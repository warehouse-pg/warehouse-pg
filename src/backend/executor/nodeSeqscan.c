/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.c
 *	  Support routines for sequential scans of relations.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSeqscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSeqScan				sequentially scans a relation.
 *		ExecSeqNext				retrieve next tuple in sequential order.
 *		ExecInitSeqScan			creates and initializes a seqscan node.
 *		ExecEndSeqScan			releases any storage allocated.
 *		ExecReScanSeqScan		rescans the relation
 *
 *		ExecSeqScanEstimate		estimates DSM space needed for parallel scan
 *		ExecSeqScanInitializeDSM initialize DSM for parallel scan
 *		ExecSeqScanReInitializeDSM reinitialize DSM for fresh parallel scan
 *		ExecSeqScanInitializeWorker attach to DSM info in parallel worker
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "cdb/cdbaocsam.h"
#include "cdb/cdbaocsam_stagedscan.h"
#include "executor/execdebug.h"
#include "executor/nodeSeqscan.h"
#include "utils/rel.h"
#include "utils/runtimefilter.h"
#include "nodes/nodeFuncs.h"

static TupleTableSlot *SeqNext(SeqScanState *node);
static void SeqScanSyncStagedInstrumentation(SeqScanState *node, TableScanDesc scandesc);
static void SeqScanInitRuntimeFilters(SeqScanState *node, List *qual);
static void SeqScanWatchRuntimeFilters(SeqScanState *node);

/*
 * WHPG: mirror what the scan's runtime filter clauses have removed into the
 * node's instrumentation (EXPLAIN ANALYZE's "Rows Removed by Runtime
 * Filter"), as an increment, since a partition's scan under a Dynamic Seq
 * Scan counts into the dynamic node's. ExecScan has counted each of these
 * rows as removed by the node's qual, which the clauses are part of; they
 * are taken out of that count again, so that "Rows Removed by Filter" stays
 * the scan's own predicate's and is absent when there is none. Called as
 * rows are fetched, so that the count is in place whenever the QE ships its
 * stats, which happens before the scan ends: a row is probed after it is
 * fetched, so every rejection before a returned row is in. Without a
 * filter, or without instrumentation, it is one test per row.
 */
static inline void
SeqScanSyncRuntimeFilterRemoved(SeqScanState *node)
{
	Instrumentation *instr;
	double		removed;

	if (likely(node->rf_probes == NIL))
		return;
	instr = node->staged_instr ? node->staged_instr : node->ss.ps.instrument;
	if (instr == NULL)
		return;
	removed = runtimefilter_probes_removed(node->rf_probes);
	if (removed > node->rf_removed_reported)
	{
		instr->rfRowsRemoved += removed - node->rf_removed_reported;
		if (node->ss.ps.instrument != NULL)
			node->ss.ps.instrument->nfiltered1 -=
				removed - node->rf_removed_reported;
		node->rf_removed_reported = removed;
	}
}

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		SeqNext
 *
 *		This is a workhorse for ExecSeqScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
SeqNext(SeqScanState *node)
{
	TableScanDesc scandesc;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * get information from the estate and scan state
	 */
	scandesc = node->ss.ss_currentScanDesc;
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;

	if (scandesc == NULL)
	{
		/*
		 * We reach here if the scan is not parallel, or if we're serially
		 * executing a scan that was planned to be parallel.
		 */
		/*
		 * GPDB: we are using table_beginscan_es in order to also initialize the
		 * scan state with the column info needed for AOCO relations. Check the
		 * comment in table_beginscan_es() for more info.
		 */
		scandesc = table_beginscan_es_staged(node->ss.ss_currentRelation,
											 estate->es_snapshot,
											 node->ss.ps.plan->targetlist,
											 node->ss.ps.plan->qual,
											 NULL,
											 NULL,
											 ((SeqScan *) node->ss.ps.plan)->staged.enabled ?
											 ((SeqScan *) node->ss.ps.plan)->staged.phaseAWidth : 0,
											 ((SeqScan *) node->ss.ps.plan)->staged.filterAttnos,
											 ((SeqScan *) node->ss.ps.plan)->staged.phaseASel);
		node->ss.ss_currentScanDesc = scandesc;

		/*
		 * WHPG: a Staged Scan evaluates the plan's qual inside the access
		 * method, in ExprContexts of its own; give it the exec Params a
		 * runtime filter clause reads (runtimefilter.c).
		 */
		if (RelationIsAoCols(scandesc->rs_rd))
			((AOCSScanDesc) scandesc)->stagedParamExecVals = estate->es_param_exec_vals;

		/*
		 * GPDB: the counts a staged scan keeps on its instrumentation start
		 * from zero on a fresh descriptor, so the high-water marks that
		 * SeqScanSyncStagedInstrumentation() compares them with must too.
		 * This matters when the node is reopened rather than rescanned:
		 * ExecEagerFreeSeqScan() ended the previous descriptor once the
		 * Append moved past this node, and a rescan of the Append brings us
		 * back here with the marks still at the previous pass's totals.
		 */
		node->staged_removed_reported = 0;
		node->staged_scanned_reported = 0;
		node->staged_survivors_reported = 0;
		node->staged_fallbacks_reported = 0;
		node->staged_blocks_decompressed_reported = 0;
		node->staged_blocks_skipped_reported = 0;
		node->staged_blocks_seeked_reported = 0;
		node->staged_rf_removed_reported = 0;

		/* GPDB: a staged scan's costlier counts are for EXPLAIN ANALYZE only */
		if (((SeqScan *) node->ss.ps.plan)->staged.enabled &&
			RelationIsAoCols(scandesc->rs_rd) &&
			((AOCSScanDesc) scandesc)->stagedInstrument != NULL &&
			(node->ss.ps.instrument != NULL || node->staged_instr != NULL))
			((AOCSScanDesc) scandesc)->stagedInstrument->staged_analyze = true;
	}

	/*
	 * WHPG: a runtime filter clause whose filter is not being probed would
	 * cost every row the qual's evaluation for nothing. Looked at every
	 * RF_ADAPT_WINDOW rows (SeqScanWatchRuntimeFilters()).
	 */
	if (node->rf_paramids != NIL &&
		((++node->rf_rows) & (RF_ADAPT_WINDOW - 1)) == 0)
		SeqScanWatchRuntimeFilters(node);

	/*
	 * get the next tuple from the table
	 */
	if (table_scan_getnextslot(scandesc, direction, slot))
	{
		SeqScanSyncStagedInstrumentation(node, scandesc);
		SeqScanSyncRuntimeFilterRemoved(node);
		return slot;
	}
	SeqScanSyncStagedInstrumentation(node, scandesc);
	SeqScanSyncRuntimeFilterRemoved(node);
	return NULL;
}

/*
 * GPDB: a staged scan keeps its counts on the scan's instrumentation;
 * mirror them into the node's so that they are in place whenever the QE
 * ships its stats (which happens before ExecEndSeqScan), including under a
 * LIMIT. The rows the predicate rejected in Phase A or a stage never reached
 * ExecScan's filter, so they are added to nfiltered1 here: "Rows Removed by
 * Filter" then reports what the predicate removed, whichever phase did it.
 * The rows a runtime filter clause rejected are among the scan's rejections
 * and have a line of their own, so they are left out of that count.
 */
static void
SeqScanSyncStagedInstrumentation(SeqScanState *node, TableScanDesc scandesc)
{
	Instrumentation *instr = node->staged_instr ? node->staged_instr
		: node->ss.ps.instrument;
	TableScanInstrumentation *scaninstr;
	double		scanned;
	double		survivors;
	double		fallbacks;
	double		blocks;
	double		removed;

	/* only an AOCO scan opened for a Staged Scan plan keeps these counts */
	if (instr == NULL || !((SeqScan *) node->ss.ps.plan)->staged.enabled ||
		!RelationIsAoCols(scandesc->rs_rd))
		return;
	scaninstr = ((AOCSScanDesc) scandesc)->stagedInstrument;
	if (scaninstr == NULL)
		return;
	stagedscan_sync_rf_removed((AOCSScanDesc) scandesc);

	/*
	 * Add the increments rather than copying the totals: a Dynamic Seq Scan
	 * points every partition's scan at its own instrumentation, and the
	 * partitions' counts have to add up there.
	 */
	scanned = (double) scaninstr->staged_rows_scanned;
	if (scanned > node->staged_scanned_reported)
	{
		instr->stagedRowsScanned += scanned - node->staged_scanned_reported;
		node->staged_scanned_reported = scanned;
	}
	survivors = (double) scaninstr->staged_survivors;
	if (survivors > node->staged_survivors_reported)
	{
		instr->stagedSurvivors += survivors - node->staged_survivors_reported;
		node->staged_survivors_reported = survivors;
	}
	fallbacks = (double) scaninstr->staged_fallbacks;
	if (fallbacks > node->staged_fallbacks_reported)
	{
		instr->stagedFallbacks += fallbacks - node->staged_fallbacks_reported;
		node->staged_fallbacks_reported = fallbacks;
	}
	blocks = (double) scaninstr->staged_blocks_decompressed;
	if (blocks > node->staged_blocks_decompressed_reported)
	{
		instr->stagedBlocksDecompressed += blocks - node->staged_blocks_decompressed_reported;
		node->staged_blocks_decompressed_reported = blocks;
	}
	blocks = (double) scaninstr->staged_blocks_skipped;
	if (blocks > node->staged_blocks_skipped_reported)
	{
		instr->stagedBlocksSkipped += blocks - node->staged_blocks_skipped_reported;
		node->staged_blocks_skipped_reported = blocks;
	}
	blocks = (double) scaninstr->staged_blocks_seeked;
	if (blocks > node->staged_blocks_seeked_reported)
	{
		instr->stagedBlocksSeeked += blocks - node->staged_blocks_seeked_reported;
		node->staged_blocks_seeked_reported = blocks;
	}
	removed = (double) scaninstr->staged_rows_removed -
		(double) scaninstr->staged_rf_rows_removed;
	if (removed > node->staged_removed_reported)
	{
		instr->nfiltered1 += removed - node->staged_removed_reported;
		node->staged_removed_reported = removed;
	}
	removed = (double) scaninstr->staged_rf_rows_removed;
	if (removed > node->staged_rf_removed_reported)
	{
		instr->rfRowsRemoved += removed - node->staged_rf_removed_reported;
		node->staged_rf_removed_reported = removed;
	}
}

/*
 * SeqRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
SeqRecheck(SeqScanState *node, TupleTableSlot *slot)
{
	/*
	 * Note that unlike IndexScan, SeqScan never use keys in heap_beginscan
	 * (and this is very bad) - so, here we do not check are keys ok or not.
	 */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecSeqScan(node)
 *
 *		Scans the relation sequentially and returns the next qualifying
 *		tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecSeqScan(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) SeqNext,
					(ExecScanRecheckMtd) SeqRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitSeqScan
 * ----------------------------------------------------------------
 */
SeqScanState *
ExecInitSeqScan(SeqScan *node, EState *estate, int eflags)
{
	Relation	currentRelation;

	/*
	 * get the relation object id from the relid'th entry in the range table,
	 * open that relation and acquire appropriate lock on it.
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	return ExecInitSeqScanForPartition(node, estate, currentRelation);
}

SeqScanState *
ExecInitSeqScanForPartition(SeqScan *node, EState *estate,
							Relation currentRelation)
{
	SeqScanState *scanstate;

	/*
	 * Once upon a time it was possible to have an outerPlan of a SeqScan, but
	 * not any more.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(SeqScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecSeqScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * open the scan relation
	 */
	scanstate->ss.ss_currentRelation = currentRelation;

	/* and create slot with the appropriate rowtype */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  RelationGetDescr(scanstate->ss.ss_currentRelation),
						  table_slot_callbacks(scanstate->ss.ss_currentRelation));

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child expressions
	 */
	/*
	 * GPDB: on a Staged Scan plan over an AOCO relation the access method
	 * applies the qual itself and returns only rows that satisfy it -- on the
	 * two-phase path, on the ordinary scan a segment file may fall back to,
	 * and when it declines the two-phase scan at open (aocs_stagedscan_getnext()).
	 * Running the qual here again would only cost the evaluation and would
	 * need columns the scan no longer fetches for the emitted row, so this
	 * node runs no qual. A partition of another access method under a
	 * dynamic scan keeps it. EvalPlanQual is not reached on AO tables; keep
	 * the qual there anyway, the access method's filter makes it harmless.
	 */
	if (node->staged.enabled && node->staged.phaseAWidth > 0 &&
		RelationIsAoCols(currentRelation) &&
		estate->es_epq_active == NULL)
		scanstate->ss.ps.qual = NULL;
	else
		scanstate->ss.ps.qual =
			ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);
	/* WHPG: the runtime filter clauses in it, left out while none is probed */
	SeqScanInitRuntimeFilters(scanstate, node->scan.plan.qual);

	return scanstate;
}

/*
 * WHPG: note the runtime filter clauses of the scan's qual (planrtfilter.c),
 * so that SeqNext can give them up once their filters are dead: the Params
 * they read, and the qual compiled without them. For a scan whose qual has
 * none, nothing is kept and SeqNext's check costs one test per row.
 */
static void
SeqScanInitRuntimeFilters(SeqScanState *node, List *qual)
{
	List	   *filters;
	List	   *rest;
	ListCell   *lc;

	node->rf_paramids = NIL;
	node->rf_resting = false;
	node->rf_allparamids = NIL;
	node->rf_fullqual = NULL;
	node->rf_lessqual = NULL;
	node->rf_rows = 0;
	node->rf_probes = NIL;
	node->rf_removed_reported = 0;

	if (node->ss.ps.qual == NULL)
		return;
	runtimefilter_split_qual(qual, &filters, &rest);
	if (filters == NIL)
		return;
	foreach(lc, filters)
		node->rf_allparamids = lappend_int(node->rf_allparamids,
										   ((RuntimeFilterExpr *) lfirst(lc))->paramid);
	node->rf_paramids = node->rf_allparamids;
	node->rf_fullqual = node->ss.ps.qual;
	node->rf_lessqual = ExecInitQual(rest, &node->ss.ps);
	node->rf_probes = runtimefilter_qual_probes(node->ss.ps.qual);
}

/*
 * Choose the scan's qual by the state of its runtime filters, every
 * RF_ADAPT_WINDOW rows. While none of them is being probed -- each is the
 * dead one, at rest, or not a filter at all, which the probe gives up on --
 * the qual without the clauses runs: none at all if nothing else was there,
 * and ExecScan takes its no-qual shortcut from the next row on. If all are
 * dead that is for good. Otherwise the rows that go by are counted off the
 * resting filters' rests here, a window at a time, since no probe sees
 * them; the full qual is back when the first of them wakes. A Param still
 * NULL is a filter yet to come (the join publishes a dead one in place of
 * none, runtimefilter_dead()), so the clause stays.
 */
static void
SeqScanWatchRuntimeFilters(SeqScanState *node)
{
	ParamExecData *vals = node->ss.ps.ps_ExprContext->ecxt_param_exec_vals;
	bool		probing = false;
	bool		resting = false;
	ListCell   *lc;

	if (vals == NULL)
		return;
	foreach(lc, node->rf_paramids)
	{
		ParamExecData *prm = &vals[lfirst_int(lc)];
		RuntimeFilter *f;

		if (prm->isnull)
		{
			probing = true;
			continue;
		}
		f = (RuntimeFilter *) runtimefilter_check((struct varlena *) DatumGetPointer(prm->value));
		if (f == NULL || f->dead)
			continue;
		if (node->rf_resting)
			runtimefilter_rest(f, RF_ADAPT_WINDOW);
		if (f->rest_rows > 0)
			resting = true;
		else
			probing = true;
	}

	if (probing)
	{
		if (node->rf_resting)
		{
			node->ss.ps.qual = node->rf_fullqual;
			node->rf_resting = false;
		}
	}
	else if (!node->rf_resting)
	{
		SeqScanSyncRuntimeFilterRemoved(node);
		node->ss.ps.qual = node->rf_lessqual;
		if (resting)
			node->rf_resting = true;
		else
			node->rf_paramids = NIL;
	}
}

/* ----------------------------------------------------------------
 *		ExecEndSeqScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndSeqScan(SeqScanState *node)
{
	TableScanDesc scanDesc;

	/*
	 * get information from node
	 */
	scanDesc = node->ss.ss_currentScanDesc;

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the tuple table
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close heap scan
	 */
	if (scanDesc != NULL)
		table_endscan(scanDesc);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecReScanSeqScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanSeqScan(SeqScanState *node)
{
	TableScanDesc scan;

	scan = node->ss.ss_currentScanDesc;

	if (scan != NULL)
		table_rescan(scan,		/* scan desc */
					 NULL);		/* new scan keys */

	/* WHPG: a rescan may bring a new runtime filter: watch the clauses again */
	if (node->rf_allparamids != NIL &&
		(node->rf_paramids == NIL || node->rf_resting))
	{
		node->ss.ps.qual = node->rf_fullqual;
		node->rf_paramids = node->rf_allparamids;
		node->rf_resting = false;
	}
	node->rf_rows = 0;

	ExecScanReScan((ScanState *) node);
}

/* ----------------------------------------------------------------
 *		ExecEagerFreeSeqScan
 *
 *		WHPG: release the scan descriptor early.  Scans of AO/AOCS
 *		tables keep sizeable private read buffers, so for an Append
 *		over many such partitions, ending each scan once it is done
 *		keeps memory proportional to one partition rather than all of
 *		them.  SeqNext() opens a fresh scan if the node is rescanned.
 *
 *		Only Append calls this, and it stops doing so once it sees a
 *		rescan.  It is deliberately not part of ExecSquelchNode(): nodes
 *		that squelch their input (Limit, HashJoin) may be rescanned once
 *		per outer row of a parameterized NestLoop, and reopening an AO
 *		scan each time costs much more than table_rescan().
 *
 *		Heap scans are left alone: they hold little memory, and a
 *		reopened heap scan may pick a different synchronized-scan start
 *		block, where table_rescan() would keep the old one.
 * ----------------------------------------------------------------
 */
void
ExecEagerFreeSeqScan(SeqScanState *node)
{
	TableScanDesc scanDesc = node->ss.ss_currentScanDesc;

	if (scanDesc == NULL ||
		!RelationIsAppendOptimized(node->ss.ss_currentRelation))
		return;

	/* A parallel scan descriptor can't be recreated lazily; keep it. */
	if (scanDesc->rs_parallel != NULL)
		return;

	/* The scan slot may point into the scan's buffers; clear it first. */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	table_endscan(scanDesc);
	node->ss.ss_currentScanDesc = NULL;
}

/* ----------------------------------------------------------------
 *						Parallel Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecSeqScanEstimate
 *
 *		Compute the amount of space we'll need in the parallel
 *		query DSM, and inform pcxt->estimator about our needs.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanEstimate(SeqScanState *node,
					ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;

	node->pscan_len = table_parallelscan_estimate(node->ss.ss_currentRelation,
												  estate->es_snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeDSM
 *
 *		Set up a parallel heap scan descriptor.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeDSM(SeqScanState *node,
						 ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;
	ParallelTableScanDesc pscan;

	pscan = shm_toc_allocate(pcxt->toc, node->pscan_len);
	table_parallelscan_initialize(node->ss.ss_currentRelation,
								  pscan,
								  estate->es_snapshot);
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id, pscan);
	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanReInitializeDSM
 *
 *		Reset shared state before beginning a fresh scan.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanReInitializeDSM(SeqScanState *node,
						   ParallelContext *pcxt)
{
	ParallelTableScanDesc pscan;

	pscan = node->ss.ss_currentScanDesc->rs_parallel;
	table_parallelscan_reinitialize(node->ss.ss_currentRelation, pscan);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeWorker
 *
 *		Copy relevant information from TOC into planstate.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeWorker(SeqScanState *node,
							ParallelWorkerContext *pwcxt)
{
	ParallelTableScanDesc pscan;

	pscan = shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, false);
	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan);
}
