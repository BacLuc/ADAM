/*-------------------------------------------------------------------------
 *
 * nodeBitmapAnd.c
 *	  routines to handle BitmapAnd nodes.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeBitmapAnd.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitBitmapAnd	- initialize the BitmapAnd node
 *		MultiExecBitmapAnd	- retrieve the result bitmap from the node
 *		ExecEndBitmapAnd	- shut down the BitmapAnd node
 *		ExecReScanBitmapAnd - rescan the BitmapAnd node
 *
 *	 NOTES
 *		BitmapAnd nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans,
 *		much like Append nodes.  The logic is much simpler than
 *		Append, however, since we needn't cope with forward/backward
 *		execution.
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeBitmapAnd.h"
#include "miscadmin.h"
#include "utils/rel.h"


/* ----------------------------------------------------------------
 *		ExecInitBitmapAnd
 *
 *		Begin all of the subscans of the BitmapAnd node.
 * ----------------------------------------------------------------
 */
BitmapAndState *
ExecInitBitmapAnd(BitmapAnd *node, EState *estate, int eflags)
{
	BitmapAndState *bitmapandstate = makeNode(BitmapAndState);
	PlanState **bitmapplanstates;
	int			nplans;
	int			i;
	ListCell   *l;
	Plan	   *initNode;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * Set up empty vector of subplan states
	 */
	nplans = list_length(node->bitmapplans);

	bitmapplanstates = (PlanState **) palloc0(nplans * sizeof(PlanState *));

	/*
	 * create new BitmapAndState for our BitmapAnd node
	 */
	bitmapandstate->ps.plan = (Plan *) node;
	bitmapandstate->ps.state = estate;
	bitmapandstate->bitmapplans = bitmapplanstates;
	bitmapandstate->nplans = nplans;

	/*
	 * Miscellaneous initialization
	 *
	 * BitmapAnd plans don't have expression contexts because they never call
	 * ExecQual or ExecProject.  They don't need any tuple slots either.
	 */

	/*
	 * call ExecInitNode on each of the plans to be executed and save the
	 * results into the array "bitmapplanstates".
	 */
	i = 0;
	foreach(l, node->bitmapplans)
	{
		initNode = (Plan *) lfirst(l);
		bitmapplanstates[i] = ExecInitNode(initNode, estate, eflags);
		i++;
	}

	return bitmapandstate;
}

/* ----------------------------------------------------------------
 *	   MultiExecBitmapAnd
 * ----------------------------------------------------------------
 */
Node *
MultiExecBitmapAnd(BitmapAndState *node)
{
	PlanState **bitmapplans;
	int			nplans;
	int			i;
	TIDBitmap  *result = NULL;
	bool		no_intersect = false;

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	/*
	 * get information from the node
	 */
	bitmapplans = node->bitmapplans;
	nplans = node->nplans;

	/* Scan subplans and change order if ADAM plan is not at the end */
	/* nb: there should be at maximum 1 ADAM plan (since only 1 distance calculation is allowed - right?)*/
	for (i = 0; i < nplans; i++){
		PlanState  *subnode = bitmapplans[i];

		if (IsA(subnode, BitmapIndexScanState) && i != nplans - 1)	{
			BitmapIndexScanState *scanState = ((BitmapIndexScanState *) subnode);

			if(scanState && scanState->biss_RelationDesc && 
				scanState->biss_RelationDesc->rd_rel->relam == VA_AM_OID){
					PlanState *tmp = bitmapplans[nplans - 1];
					bitmapplans[nplans - 1] = bitmapplans[i];
					bitmapplans[i] = tmp;
			}
		}
	}

	/*
	 * Scan all the subplans and AND their result bitmaps
	 */
	for (i = 0; i < nplans; i++)
	{
		PlanState  *subnode = bitmapplans[i];
		TIDBitmap  *subresult;
		no_intersect = false;

		/* ADAM */
		if (IsA(subnode, BitmapIndexScanState))	{
			BitmapIndexScanState *scanState = ((BitmapIndexScanState *) subnode);
			
			if(scanState && scanState->biss_RelationDesc && 
				scanState->biss_RelationDesc->rd_rel->relam == VA_AM_OID){
					if (result == NULL){
						/* XXX should we use less than work_mem for this? */
						result = tbm_create(work_mem * 1024L);
					}
			
					scanState->biss_result = result;

					/* using a VA index */
					scanState->adamScanClause = node->ps.plan->adamPlanClause;

					if(scanState->adamScanClause){
						AdamScanClause *adamScanClause = (AdamScanClause *) scanState->adamScanClause;

						/* 
						* we have a search with a WHERE clause given (besides the one that we cheat in,
						* to bring postgres to use our VA index) 
						*/
						adamScanClause->check_tid = true;

						/* 
						* if unfortunately the where clause is not at first, then set the limit to -1, so that
						* all the results are returned (this is a costly index scanning, but not much worse than going sequential) 
						* and avoid a check TID, because we return all entries anyways
						* NB: this should not happen because we changed the order in the code above, but we should keep this code
						* to really make sure
						*/
						if(i != 0){
							adamScanClause->nn_limit = node->limit;
						} else {
							adamScanClause->nn_limit = -1;
							adamScanClause->check_tid = false;
						}

						no_intersect = true;
					}
			}
		}

		subresult = (TIDBitmap *) MultiExecProcNode(subnode);

		if (!subresult || !IsA(subresult, TIDBitmap))
			elog(ERROR, "unrecognized result from subplan");

		if (result == NULL || no_intersect)
			result = subresult; /* first subplan */
		else
		{
			tbm_intersect(result, subresult);
			tbm_free(subresult);
		}

		/*
		 * If at any stage we have a completely empty bitmap, we can fall out
		 * without evaluating the remaining subplans, since ANDing them can no
		 * longer change the result.  (Note: the fact that indxpath.c orders
		 * the subplans by selectivity should make this case more likely to
		 * occur.)
		 */
		if (tbm_is_empty(result))
			break;
	}

	if (result == NULL)
		elog(ERROR, "BitmapAnd doesn't support zero inputs");

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStopNode(node->ps.instrument, 0 /* XXX */ );

	return (Node *) result;
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapAnd
 *
 *		Shuts down the subscans of the BitmapAnd node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapAnd(BitmapAndState *node)
{
	PlanState **bitmapplans;
	int			nplans;
	int			i;

	/*
	 * get information from the node
	 */
	bitmapplans = node->bitmapplans;
	nplans = node->nplans;

	/*
	 * shut down each of the subscans (that we've initialized)
	 */
	for (i = 0; i < nplans; i++)
	{
		if (bitmapplans[i])
			ExecEndNode(bitmapplans[i]);
	}
}

void
ExecReScanBitmapAnd(BitmapAndState *node)
{
	int			i;

	for (i = 0; i < node->nplans; i++)
	{
		PlanState  *subnode = node->bitmapplans[i];

		/*
		 * ExecReScan doesn't know about my subplans, so I have to do
		 * changed-parameter signaling myself.
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * If chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}
}
