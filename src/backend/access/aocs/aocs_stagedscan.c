/*-------------------------------------------------------------------------
 *
 * aocs_stagedscan.c
 *	  Staged Scan: two-phase scan for AOCO tables.
 *
 * This file implements Phase A -- the filter-column-only scan that produces
 * the surviving row numbers. Phase B (header-scan-and-skip materialization)
 * and the executor entry point live in aocsam.c, which owns the scan
 * descriptor.
 *
 * See cdbaocsam_stagedscan.h and README.staged_scan.md in this directory.
 *
 * Portions Copyright (c) 2026, WarehousePG contributors.
 *
 * IDENTIFICATION
 *	    src/backend/access/aocs/aocs_stagedscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/aomd.h"
#include "access/aocssegfiles.h"
#include "access/appendonly_visimap.h"
#include "access/appendonlytid.h"
#include "access/appendonlywriter.h"
#include "access/stratnum.h"
#include "catalog/aocatalog.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "catalog/pg_attribute_encoding.h"
#include "cdb/cdbaocsam.h"
#include "cdb/cdbaocsam_stagedscan.h"
#include "cdb/cdbappendonlyam.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/datumstream.h"
#include "utils/faultinjector.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

static void stagedscan_compile_simple(StagedScanState *gi, List *quals,
									  TupleDesc tupdesc);

/*
 * The first-batch density check (stagedscan_survivors_too_dense()) is only
 * made once Phase A has entered this many of its column's varblocks, and it
 * falls back when Phase B is estimated to cost at least this fraction of the
 * ordinary scan: a gain below 10% is not worth the risk of a loss.
 */
#define STAGED_DENSITY_MIN_BLOCKS	64
#define STAGED_DENSITY_FALLBACK_RATIO	0.9

/*
 * A multi-stage predicate falls back when the first batch shows Phase A
 * keeping more than this many times the planner's estimate of it, and more
 * than this multiple of whpg_staged_scan_min_selectivity: statistics that far
 * off (a clause on an expression with no statistics is estimated at 0.5%
 * whatever it keeps) leave the later stages a survivor set the cost model
 * never saw. Estimates off by less are what the stages themselves absorb,
 * and what the run-time reordering corrects.
 */
#define STAGED_ESTIMATE_SLACK		4.0
#define STAGED_ESTIMATE_MIN_GATE_MULT	2.0

/*
 * Survivor batches start small and double up to STAGEDSCAN_BATCH_MAX_ROWS:
 * Phase B emits its first row once the first batch is in, so a small first
 * batch is what bounds the time to the first row (a LIMIT, an EXISTS, a
 * cursor fetching a few rows), while the later, larger batches keep the
 * per-batch bookkeeping negligible over a full scan. The array grows with
 * the batches and never beyond the largest one: 65536 row numbers are half
 * a megabyte, plus a carried datum each.
 */
#define STAGEDSCAN_BATCH_FIRST_ROWS			8
#define STAGEDSCAN_BATCH_MAX_ROWS			65536
#define STAGEDSCAN_SURV_INITIAL_CAPACITY	STAGEDSCAN_BATCH_FIRST_ROWS

/*
 * The run-time reordering of the Phase B stages (see
 * stagedscan_adapt_stage_order()) re-scores the stages after the first
 * STAGED_ADAPT_INITIAL_WINDOW survivors and then after each window twice the
 * previous; a stage reached by fewer than STAGED_ADAPT_MIN_STAGE_ROWS rows in
 * a window takes no part in a swap.
 */
#define STAGED_ADAPT_INITIAL_WINDOW		4096
#define STAGED_ADAPT_MIN_STAGE_ROWS		1000

/*
 * Point one of Phase A's streams at a column's segment file.
 *
 * open_datumstreamread_segfile() wants the relation's base path, which the
 * ordinary scan computes once for all of its columns; here there is only ever
 * the one column, so compute it and let it go again.
 */
static void
stagedscan_open_segfile(AOCSScanDesc scan, AOCSFileSegInfo *segInfo,
						DatumStreamRead *ds, AttrNumber attno)
{
	Relation	rel = scan->rs_base.rs_rd;
	char	   *basepath = relpathbackend(rel->rd_node, rel->rd_backend, MAIN_FORKNUM);

	open_datumstreamread_segfile(basepath, rel, segInfo, ds, attno);
	pfree(basepath);
}

/*
 * Open a DatumStreamRead for a single column, independent of the scan's
 * columnScanInfo.ds[] array.
 *
 * Mirrors open_ds_read() in aocsam.c, but for exactly one column, because
 * Phase A must be able to read the filter column while Phase B independently
 * materializes that same column.
 */
static DatumStreamRead *
stagedscan_open_filter_stream(AOCSScanDesc scan, AOCSFileSegInfo *segInfo,
							  AttrNumber attno)
{
	Relation	rel = scan->rs_base.rs_rd;
	TupleDesc	tupdesc = scan->columnScanInfo.relationTupleDesc;
	Form_pg_attribute attr = TupleDescAttr(tupdesc, attno);
	StdRdOptions **opts;
	DatumStreamRead *ds;
	StringInfoData titleBuf;

	opts = RelationGetAttributeOptions(rel);
	if (opts[attno] == NULL || opts[attno]->blocksize == 0)
		elog(ERROR, "could not find blocksize option for AOCO column in pg_attribute_encoding");

	initStringInfo(&titleBuf);
	appendStringInfo(&titleBuf,
					 "Staged Scan filter scan of Append-Only Column-Oriented relation '%s', column #%d '%s'",
					 RelationGetRelationName(rel),
					 attno + 1,
					 NameStr(attr->attname));

	ds = create_datumstreamread(opts[attno]->compresstype,
								opts[attno]->compresslevel,
								scan->checksum,
								opts[attno]->blocksize,
								attr,
								RelationGetRelationName(rel),
								titleBuf.data);

	for (int i = 0; i < RelationGetNumberOfAttributes(rel); i++)
		if (opts[i])
			pfree(opts[i]);
	pfree(opts);

	stagedscan_open_segfile(scan, segInfo, ds, attno);

	return ds;
}

/*
 * Append one surviving row number to the current batch, with the Phase A
 * column's datum for it when that column is carried (gi->carryAttno >= 0).
 *
 * Returns false if the batch is already full; Phase A stops filling a batch
 * before that happens, so this is a guard, not a policy.
 */
static bool
stagedscan_surv_append(StagedScanState *gi, int64 rowNum,
					   Datum value, bool isnull)
{
	StagedScanSurvivors *s = &gi->surv;
	bool		carry = (gi->carryAttno >= 0);

	if (s->nrows >= STAGEDSCAN_BATCH_MAX_ROWS)
		return false;

	if (s->nrows == s->capacity)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(gi->ctx);
		int64		newcap;

		if (s->capacity == 0)
			newcap = STAGEDSCAN_SURV_INITIAL_CAPACITY;
		else
			newcap = s->capacity * 2;

		if (newcap > STAGEDSCAN_BATCH_MAX_ROWS)
			newcap = STAGEDSCAN_BATCH_MAX_ROWS;

		if (s->rownums == NULL)
			s->rownums = (int64 *) palloc(sizeof(int64) * newcap);
		else
			s->rownums = (int64 *) repalloc(s->rownums, sizeof(int64) * newcap);
		if (carry)
		{
			if (s->values == NULL)
			{
				s->values = (Datum *) palloc(sizeof(Datum) * newcap);
				s->isnull = (bool *) palloc(sizeof(bool) * newcap);
			}
			else
			{
				s->values = (Datum *) repalloc(s->values, sizeof(Datum) * newcap);
				s->isnull = (bool *) repalloc(s->isnull, sizeof(bool) * newcap);
			}
		}

		s->capacity = newcap;
		MemoryContextSwitchTo(oldctx);
	}

	/* Phase A walks the segfile forward, so this must stay ascending. */
	Assert(s->nrows == 0 || s->rownums[s->nrows - 1] < rowNum);

	if (carry)
	{
		s->values[s->nrows] = value;
		s->isnull[s->nrows] = isnull;
	}
	s->rownums[s->nrows++] = rowNum;
	return true;
}

/*
 * The qual as a flat list of conjuncts.
 *
 * The Postgres planner hands a scan its restrictions that way; GPORCA hands
 * over its filter as one AND. Each stage evaluates conjuncts, so an AND left
 * whole would be settled only at the last stage among all its columns and
 * leave Phase A nothing to evaluate. Nested ANDs are flattened too.
 */
List *
stagedscan_flatten_qual(List *qual)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, qual)
	{
		Node	   *clause = (Node *) lfirst(lc);

		if (is_andclause(clause))
			result = list_concat(result,
								 stagedscan_flatten_qual(((BoolExpr *) clause)->args));
		else
			result = lappend(result, clause);
	}
	return result;
}

/*
 * Does the expression contain a Param?
 *
 * The stage quals are compiled with no parent PlanState and evaluated in a
 * standalone ExprContext, which carries neither the external parameter list
 * of a prepared statement's generic plan nor the exec-params a nested loop
 * feeds an inner scan. Either kind fails at evaluation time with "no value
 * found for parameter", so such quals are declined up front. The planner
 * declines them too; this is the executor's own guard.
 */
static bool
stagedscan_contains_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
		return true;
	return expression_tree_walker(node, stagedscan_contains_param_walker, context);
}

bool
stagedscan_contains_param(Node *node)
{
	return stagedscan_contains_param_walker(node, NULL);
}

/* Does the expression reference a system column or a whole row? */
static bool
stagedscan_contains_sysattr_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) && ((Var *) node)->varattno <= 0)
		return true;
	return expression_tree_walker(node, stagedscan_contains_sysattr_walker, context);
}

static bool
stagedscan_contains_sysattr(Node *node)
{
	return stagedscan_contains_sysattr_walker(node, NULL);
}

/*
 * Can evaluating the clause fail on some row -- raise an error, say -- where
 * it would not on another?
 *
 * A leakproof function promises exactly that it cannot: it reveals nothing
 * about its arguments but its result, and an error for some values and not
 * others would. A clause made only of leakproof calls and of node types that
 * call nothing gives the same outcome whatever row it is shown; any other
 * clause (a cast, integer arithmetic, a division) must not be shown a row the
 * ordinary scan would not show it. Node types not listed are treated as able
 * to fail. Unlike contain_leaked_vars(), a call with no Var below it counts
 * too: whether it fails does not depend on the row, but whether it runs at
 * all does.
 */
static bool
stagedscan_can_fail_checker(Oid func_id, void *context)
{
	return !get_func_leakproof(func_id);
}

static bool
stagedscan_can_fail_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_ArrayExpr:
		case T_BoolExpr:
		case T_RelabelType:
		case T_CollateExpr:
		case T_CaseExpr:
		case T_CaseTestExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_List:
			break;

		case T_FuncExpr:
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
		case T_ScalarArrayOpExpr:
		case T_CoerceViaIO:
		case T_ArrayCoerceExpr:
			if (check_functions_in_node(node, stagedscan_can_fail_checker, context))
				return true;
			break;

		default:
			return true;
	}
	return expression_tree_walker(node, stagedscan_can_fail_walker, context);
}

bool
stagedscan_clause_can_fail(Node *clause)
{
	return stagedscan_can_fail_walker(clause, NULL);
}

/*
 * Does a stage order keep every clause that can fail behind the clauses the
 * ordinary scan evaluates before it?
 *
 * The ordinary scan evaluates the qual in list order and stops at the first
 * clause that is not true, so it shows a clause only the rows every clause
 * before it in the list passed. A Staged Scan decides each clause at its
 * stage ('clauseStage', from stagedscan_assign_clauses()) and, within a
 * stage, in list order. A clause that cannot fail may move anywhere: the
 * outcome is the same on any row. A clause that can fail must be decided no
 * earlier than every clause before it in the list, so that it too sees only
 * rows they all passed. (Phase A additionally consults the visimap before
 * such a clause; see stagedscan_run_phase_a().) 'canFail' is per clause.
 */
static bool
stagedscan_order_safe(int nclauses, const int *clauseStage, const bool *canFail)
{
	int			latest = 0;		/* latest stage among the clauses so far */

	for (int i = 0; i < nclauses; i++)
	{
		if (canFail[i] && clauseStage[i] < latest)
			return false;
		latest = Max(latest, clauseStage[i]);
	}
	return true;
}

/*
 * Is the OR clause evaluated disjunct by disjunct (the bypass)? Not if it can
 * fail: the ordinary scan evaluates its disjuncts left to right and stops at
 * the first true one, so splitting them across stages could evaluate a
 * disjunct the ordinary scan never reaches.
 */
static bool
stagedscan_split_or(Node *clause, bool canFail)
{
	return is_orclause(clause) && !canFail;
}

/*
 * Turn the planner's one-based stage list into a zero-based array, checking
 * that every entry names a live, distinct column. Returns the stage count, or
 * -1 if the list is unusable.
 */
static int
stagedscan_stage_array(List *filterAttnos, TupleDesc tupdesc,
					   AttrNumber *stageAttnos, int maxstages)
{
	int			nstages = 0;
	ListCell   *lc;

	foreach(lc, filterAttnos)
	{
		int			attno = lfirst_int(lc) - 1;

		if (attno < 0 || attno >= tupdesc->natts || nstages >= maxstages)
			return -1;
		if (TupleDescAttr(tupdesc, attno)->attisdropped)
			return -1;
		for (int i = 0; i < nstages; i++)
			if (stageAttnos[i] == attno)
				return -1;
		stageAttnos[nstages++] = attno;
	}

	return nstages;
}

/*
 * Decide which stage decides each clause of 'qual'.
 *
 * A clause is decided at the *last* stage among the columns it references,
 * because that is the first moment every column it needs is in the slot.
 * That covers a clause spanning two columns (a + b > 10) without any special
 * case. A clause referencing no column of the relation is decided at stage 0.
 *
 * A clause touching a column that is not a stage at all gets stage -1.
 * stagedscan_applicable() declines such a plan: the scan node runs no qual
 * of its own on a staged scan, so a clause no stage decides would go
 * unenforced. The planner stages every column the predicate references, so
 * this only guards against the plan's qual and the planner's restriction
 * list disagreeing.
 *
 * Returns the number of clauses decided at stage 0.
 */
static int
stagedscan_assign_clauses(List *qual, const AttrNumber *stageAttnos,
						  int nstages, int phaseAWidth, AttrNumber natts,
						  int *clauseStage)
{
	bool	   *cols = (bool *) palloc(sizeof(bool) * natts);
	int			i = 0;
	int			nstage0 = 0;
	ListCell   *lc;

	foreach(lc, qual)
	{
		Node	   *clause = (Node *) lfirst(lc);
		int			stage = 0;

		memset(cols, 0, sizeof(bool) * natts);
		extractcolumns_from_node(clause, cols, natts);

		for (AttrNumber a = 0; a < natts && stage >= 0; a++)
		{
			int			s;

			if (!cols[a])
				continue;
			for (s = 0; s < nstages; s++)
				if (stageAttnos[s] == a)
					break;
			if (s == nstages)
				stage = -1;		/* not a stage column: not applicable */
			else if (s > stage)
				stage = s;
		}

		/*
		 * The first phaseAWidth positions of the stage list are read together
		 * by Phase A, so a clause confined to them is decided there (stage 0)
		 * and every later position is one Phase B stage.
		 */
		if (stage >= 0)
			stage = (stage < phaseAWidth) ? 0 : stage - phaseAWidth + 1;

		clauseStage[i++] = stage;
		if (stage == 0)
			nstage0++;
	}

	pfree(cols);
	return nstage0;
}

/*
 * The stage at which a column is in the slot: a Phase B stage column at its
 * own stage; a Phase A column only once some stage re-fetches it, so it does
 * not bind the earliest stage a clause can be evaluated at (0 here). -1 if the
 * column is not a stage column at all.
 */
static int
stagedscan_column_stage(const StagedScanState *gi, AttrNumber attno)
{
	for (int p = 0; p < gi->npos; p++)
		if (gi->stageAttnos[p] == attno)
			return p < gi->nphaseA ? 0 : p - gi->nphaseA + 1;
	return -1;
}

/*
 * The stage at which one part of a clause -- a whole clause, or one disjunct
 * of an OR clause -- becomes evaluable: its latest stage column, and never
 * before the clause's own stage 's' unless it is a disjunct. 'cols' is left
 * holding the part's columns.
 */
static int
stagedscan_part_stage(const StagedScanState *gi, Node *part, bool isOr, int s,
					  bool *cols, AttrNumber natts)
{
	int			e = isOr ? 1 : s;

	memset(cols, 0, sizeof(bool) * natts);
	extractcolumns_from_node(part, cols, natts);
	for (AttrNumber a = 0; a < natts; a++)
		if (cols[a])
			e = Max(e, stagedscan_column_stage(gi, a));
	return e;
}

/*
 * Build the Phase B plan for the current stage order: which clause (or
 * disjunct of an OR clause) each stage evaluates, which columns it fetches
 * and for which rows, and which projected columns are left for the end.
 *
 * Called from stagedscan_begin() and again whenever the adaptive reordering
 * changes the Phase B order, so everything it allocates lives in stageCtx,
 * which is reset first.
 */
static void
stagedscan_build_stages(StagedScanState *gi, AOCSScanDesc scan)
{
	Relation	rel = scan->rs_base.rs_rd;
	TupleDesc	tupdesc = scan->columnScanInfo.relationTupleDesc;
	AttrNumber	natts = tupdesc->natts;
	int			k0 = gi->nphaseA;
	int			nstages = gi->npos - k0 + 1;
	int			nclauses = list_length(gi->qual);
	int		   *clauseStage;
	List	   *phaseAQuals = NIL;
	bool	   *needAlways;		/* per column: fetched whenever its stage runs */
	List	  **needOr;			/* per column: OR clauses that alone need it */
	bool	   *fetched;
	int		   *firstNeed;		/* per Phase A column: first stage reading it */
	bool	   *cols;
	int			maxEvals;
	int			i;
	ListCell   *lc;
	MemoryContext oldctx;

	MemoryContextReset(gi->stageCtx);
	oldctx = MemoryContextSwitchTo(gi->stageCtx);

	gi->nstages = nstages;
	gi->stages = (StagedScanStage *) palloc0(sizeof(StagedScanStage) * nstages);
	gi->stages[0].attno = gi->stageAttnos[0];
	for (i = 1; i < nstages; i++)
		gi->stages[i].attno = gi->stageAttnos[k0 + i - 1];

	/* Which stage settles each clause. */
	clauseStage = (int *) palloc(sizeof(int) * Max(nclauses, 1));
	if (stagedscan_assign_clauses(gi->qual, gi->stageAttnos, gi->npos, k0, natts,
								  clauseStage) == 0)
		elog(ERROR, "staged scan: no clause for Phase A on table \"%s\"",
			 RelationGetRelationName(rel));
	/* stagedscan_applicable() and the adaptive reordering both made sure */
	Assert(stagedscan_order_safe(nclauses, clauseStage, gi->clauseCanFail));

	gi->nclauses = nclauses;
	gi->clauses = (StagedScanClause *) palloc0(sizeof(StagedScanClause) * Max(nclauses, 1));
	gi->clauseDecided = (bool *) palloc0(sizeof(bool) * Max(nclauses, 1));

	maxEvals = nclauses;
	foreach(lc, gi->qual)
		if (is_orclause(lfirst(lc)))
			maxEvals += list_length(((BoolExpr *) lfirst(lc))->args);

	needAlways = (bool *) palloc0(sizeof(bool) * natts);
	needOr = (List **) palloc0(sizeof(List *) * natts);
	fetched = (bool *) palloc0(sizeof(bool) * natts);
	firstNeed = (int *) palloc(sizeof(int) * natts);
	cols = (bool *) palloc(sizeof(bool) * natts);

	for (i = 1; i < nstages; i++)
	{
		StagedScanStage *st = &gi->stages[i];

		st->fetchAttnos = (AttrNumber *) palloc(sizeof(AttrNumber) * natts);
		st->fetchNeededBy = (List **) palloc0(sizeof(List *) * natts);
		st->fetchAttnos[st->nfetch++] = st->attno;
		fetched[st->attno] = true;
		st->evals = (StagedScanEval *) palloc(sizeof(StagedScanEval) * maxEvals);
	}

	/*
	 * A column the target list (or anything else but the qual) reads must be
	 * in the slot when the row is emitted. The scan's own projection is not
	 * the right set: it includes every qual column too.
	 */
	if (scan->stagedTargetCols != NULL)
		memcpy(needAlways, scan->stagedTargetCols, sizeof(bool) * natts);
	else
		for (i = 0; i < scan->columnScanInfo.num_proj_atts; i++)
			needAlways[scan->columnScanInfo.proj_atts[i]] = true;

	/*
	 * Find the first stage that reads each Phase A column: that stage fetches
	 * it, and the filter slot keeps it for the stages after. The clauses come
	 * in qual order, not stage order, so this has to be settled before any of
	 * them is placed; otherwise a later stage that happens to come first in
	 * the qual would claim the fetch and an earlier stage would evaluate its
	 * clause on a column not yet fetched for the row.
	 */
	for (AttrNumber a = 0; a < natts; a++)
		firstNeed[a] = nstages;
	i = 0;
	foreach(lc, gi->qual)
	{
		Node	   *clause = (Node *) lfirst(lc);
		bool		isOr = stagedscan_split_or(clause, gi->clauseCanFail[i]);
		int			s = clauseStage[i++];
		List	   *parts;
		ListCell   *plc;

		if (s <= 0)
			continue;
		parts = isOr ? ((BoolExpr *) clause)->args : list_make1(clause);
		foreach(plc, parts)
		{
			int			e = stagedscan_part_stage(gi, (Node *) lfirst(plc),
												  isOr, s, cols, natts);

			for (AttrNumber a = 0; a < natts; a++)
				if (cols[a] && stagedscan_column_stage(gi, a) == 0)
					firstNeed[a] = Min(firstNeed[a], e);
		}
	}

	/*
	 * Place each clause. A clause settled at stage s is evaluated there as a
	 * whole; an OR clause is instead evaluated disjunct by disjunct, each at
	 * the first stage that has all of its columns, and a true disjunct settles
	 * the clause for the row. Any Phase A column a clause needs is fetched at
	 * the first stage that needs it (firstNeed): Phase A's datums are gone by
	 * then.
	 */
	gi->phaseAMayFail = false;
	i = 0;
	foreach(lc, gi->qual)
	{
		Node	   *clause = (Node *) lfirst(lc);
		int			s = clauseStage[i];
		StagedScanClause *cl = &gi->clauses[i];
		List	   *parts;
		int			nparts;
		int		   *partStage;
		int			lastPart = -1;
		ListCell   *plc;
		int			j;

		i++;
		cl->expr = clause;
		cl->decideStage = s;
		if (s < 0)
			elog(ERROR, "staged scan: clause of table \"%s\" references a column no stage decides",
				 RelationGetRelationName(rel));
		if (s == 0)
		{
			/*
			 * Decided in Phase A. Its columns are needed later only if the
			 * target list or a later clause asks for them.
			 */
			phaseAQuals = lappend(phaseAQuals, clause);
			if (gi->clauseCanFail[i - 1])
				gi->phaseAMayFail = true;
			continue;
		}

		cl->isOr = stagedscan_split_or(clause, gi->clauseCanFail[i - 1]);
		parts = cl->isOr ? ((BoolExpr *) clause)->args : list_make1(clause);
		nparts = list_length(parts);
		cl->ndisj = cl->isOr ? nparts : 0;
		partStage = (int *) palloc(sizeof(int) * nparts);

		/* The stage each part becomes evaluable at: its latest stage column. */
		j = 0;
		foreach(plc, parts)
		{
			int			e = stagedscan_part_stage(gi, (Node *) lfirst(plc),
												  cl->isOr, s, cols, natts);

			Assert(e >= 1 && e <= s);
			partStage[j] = e;
			if (lastPart < 0 || e >= partStage[lastPart])
				lastPart = j;
			j++;
		}

		j = 0;
		foreach(plc, parts)
		{
			Node	   *part = (Node *) lfirst(plc);
			int			e = partStage[j];
			StagedScanStage *st = &gi->stages[e];
			StagedScanEval *ev = &st->evals[st->nevals++];

			ev->clause = i - 1;
			ev->disjunct = cl->isOr ? j : -1;
			ev->last = (j == lastPart);
			/*
			 * parent is NULL because Staged Scan rejects quals containing
			 * subplans or anything else needing a PlanState.
			 */
			ev->qual = ExecInitQual(list_make1(part), NULL);

			/* the part's columns: fetch Phase A ones here, mark who needs each */
			memset(cols, 0, sizeof(bool) * natts);
			extractcolumns_from_node(part, cols, natts);
#ifdef USE_ASSERT_CHECKING
			ev->cols = (bool *) palloc(sizeof(bool) * natts);
			memcpy(ev->cols, cols, sizeof(bool) * natts);
#endif
			for (AttrNumber a = 0; a < natts; a++)
			{
				if (!cols[a])
					continue;
				if (!fetched[a])
				{
					StagedScanStage *fst = &gi->stages[firstNeed[a]];

					Assert(stagedscan_column_stage(gi, a) == 0);
					Assert(firstNeed[a] <= e);
					fst->fetchAttnos[fst->nfetch++] = a;
					fetched[a] = true;
				}
				if (cl->isOr)
					needOr[a] = list_append_unique_int(needOr[a], i - 1);
				else
					needAlways[a] = true;
			}
			j++;
		}
		pfree(partStage);
	}

	/*
	 * Now that every clause has spoken, decide per fetched column whether it
	 * is fetched for every row or only while one of its OR clauses is still
	 * open. A column no clause claims cannot occur (stagedscan_applicable()
	 * requires every stage column to be referenced by the qual), but is
	 * treated as always fetched to stay on the safe side.
	 *
	 * Nothing downstream re-evaluates the qual on the emitted row (the scan
	 * node drops its qual for a staged scan), so a column only OR
	 * clauses need can be left unfetched once they are settled; the target
	 * list's columns and those a staged non-OR clause evaluates are what
	 * needAlways[] keeps.
	 */
	for (i = 1; i < nstages; i++)
	{
		StagedScanStage *st = &gi->stages[i];

		for (int j = 0; j < st->nfetch; j++)
		{
			AttrNumber	a = st->fetchAttnos[j];

			st->fetchNeededBy[j] = (needAlways[a] || needOr[a] == NIL) ? NIL : needOr[a];
		}
	}

	gi->phaseAQual = phaseAQuals != NIL ? ExecInitQual(phaseAQuals, NULL) : NULL;
	Assert(gi->phaseAQual != NULL);
	stagedscan_compile_simple(gi, phaseAQuals, tupdesc);
	/*
	 * The fast path tests the datums before consulting the visimap, which is
	 * only safe because every comparison it admits is leakproof.
	 */
	Assert(gi->nsimple == 0 || !gi->phaseAMayFail);

	/*
	 * The columns the scan's consumers read and no stage fetched are fetched
	 * once every stage has passed. A column only the qual needed -- the Phase
	 * A column, most often -- is not among them: the scan node runs no qual
	 * on a staged scan, so nothing reads it from the emitted row.
	 */
	gi->restAttnos = (AttrNumber *)
		palloc(sizeof(AttrNumber) * Max(scan->columnScanInfo.num_proj_atts, 1));
	gi->nrest = 0;
	for (i = 0; i < scan->columnScanInfo.num_proj_atts; i++)
	{
		AttrNumber	attno = scan->columnScanInfo.proj_atts[i];

		if (fetched[attno])
			continue;
		if (scan->stagedTargetCols != NULL && !scan->stagedTargetCols[attno])
			continue;
		gi->restAttnos[gi->nrest++] = attno;
	}

	pfree(cols);
	pfree(fetched);
	pfree(firstNeed);
	pfree(needAlways);
	pfree(needOr);
	pfree(clauseStage);

	MemoryContextSwitchTo(oldctx);
}

StagedScanState *
stagedscan_begin(AOCSScanDesc scan, List *filterAttnos, int phaseAWidth,
				 List *qual, ExprContext *econtext, MemoryContext ctx)
{
	StagedScanState *gi;
	MemoryContext oldctx;
	TupleDesc	tupdesc = scan->columnScanInfo.relationTupleDesc;
	AttrNumber	natts;
	AttrNumber	stageAttnos[MaxHeapAttributeNumber];
	int			npos;

	Assert(tupdesc != NULL);
	natts = tupdesc->natts;

	npos = stagedscan_stage_array(filterAttnos, tupdesc, stageAttnos,
								  lengthof(stageAttnos));
	if (npos <= 0 || phaseAWidth < 1 || phaseAWidth > npos)
		elog(ERROR, "staged scan: invalid stage list for table \"%s\"",
			 RelationGetRelationName(scan->rs_base.rs_rd));

	oldctx = MemoryContextSwitchTo(ctx);

	gi = (StagedScanState *) palloc0(sizeof(StagedScanState));
	gi->econtext = econtext;
	gi->ctx = ctx;
	gi->ncols = natts;
	gi->colCursor = (StagedScanColumnCursor *)
		palloc0(sizeof(StagedScanColumnCursor) * natts);

	gi->npos = npos;
	gi->stageAttnos = (AttrNumber *) palloc(sizeof(AttrNumber) * npos);
	memcpy(gi->stageAttnos, stageAttnos, sizeof(AttrNumber) * npos);
	gi->qual = qual;

	gi->nphaseA = phaseAWidth;
	gi->phaseASel = scan->stagedPhaseASel;
	gi->phaseAAttnos = (AttrNumber *) palloc(sizeof(AttrNumber) * phaseAWidth);
	memcpy(gi->phaseAAttnos, stageAttnos, sizeof(AttrNumber) * phaseAWidth);
	gi->filterDs = (DatumStreamRead **) palloc0(sizeof(DatumStreamRead *) * phaseAWidth);

	/*
	 * When the target list or a later stage's clause wants the Phase A
	 * column, Phase B would have to walk its segfile a second time. Rather
	 * than that, a single by-value Phase A column rides along with the row
	 * numbers: Phase A has the datum in hand when it records the survivor,
	 * and a by-value datum stays valid after the stream moves on. A
	 * pass-by-reference column (text) or a wide Phase A is fetched by Phase B
	 * as any other column, and a Phase A column nobody asks for is fetched by
	 * no one.
	 */
	gi->carryAttno = -1;
	if (phaseAWidth == 1)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, stageAttnos[0]);

		if (att->attbyval && att->attlen > 0)
			gi->carryAttno = stageAttnos[0];
	}

	gi->clauseCanFail = (bool *) palloc(sizeof(bool) * Max(list_length(qual), 1));
	{
		int			i = 0;
		ListCell   *lc;

		foreach(lc, qual)
			gi->clauseCanFail[i++] = stagedscan_clause_can_fail((Node *) lfirst(lc));
	}
#ifdef USE_ASSERT_CHECKING
	gi->filterStamp = (int64 *) palloc0(sizeof(int64) * natts);
	gi->outStamp = (int64 *) palloc0(sizeof(int64) * natts);
#endif

	gi->stageCtx = AllocSetContextCreate(ctx, "Staged Scan stages",
										 ALLOCSET_DEFAULT_SIZES);
	stagedscan_build_stages(gi, scan);

	/*
	 * The quals reference columns by their attribute number in the relation's
	 * descriptor, so the slot has to have the full descriptor even though only
	 * the stage columns are ever populated. It is stored once here and its
	 * values overwritten in place per row; nothing ever reads a column that
	 * the current row's stages have not written.
	 */
	gi->filterSlot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	ExecClearTuple(gi->filterSlot);
	memset(gi->filterSlot->tts_isnull, true, sizeof(bool) * natts);
	gi->filterSlot->tts_nvalid = natts;
	ExecStoreVirtualTuple(gi->filterSlot);

	/*
	 * Open the block directory for Phase B's seeks, if the table has one (it
	 * is built with the table's first index, or asked for with the
	 * blockdirectory storage option) and this snapshot sees it: the
	 * transaction that created the directory filled it in for the rows
	 * already there, so an older snapshot sees none of those entries, and
	 * AppendOnlyBlockDirectory_Init_forSearch() refuses it. Phase B has
	 * nothing to lose there: it walks headers as it does on a table without
	 * one. Even a visible directory may lag the segfile tail (entries are
	 * flushed a minipage at a time), so a lookup can miss; Phase B then walks
	 * headers from where it is.
	 */
	if (AORelationBlockDirectoryIsVisible(scan->rs_base.rs_rd,
										  scan->appendOnlyMetaDataSnapshot))
	{
		bool	   *proj = (bool *) palloc0(sizeof(bool) * natts);

		for (int i = 0; i < scan->columnScanInfo.num_proj_atts; i++)
			proj[scan->columnScanInfo.proj_atts[i]] = true;

		gi->blkdir = (AppendOnlyBlockDirectory *) palloc0(sizeof(AppendOnlyBlockDirectory));
		AppendOnlyBlockDirectory_Init_forSearch(gi->blkdir,
												scan->appendOnlyMetaDataSnapshot,
												(FileSegInfo **) scan->seginfo,
												scan->total_seg,
												scan->rs_base.rs_rd,
												natts,
												true,
												proj);
		if (gi->blkdir->blkdirRel == NULL)
		{
			pfree(gi->blkdir);
			gi->blkdir = NULL;
		}
		pfree(proj);
	}

	MemoryContextSwitchTo(oldctx);

	return gi;
}

void
stagedscan_end(StagedScanState *gi)
{
	if (gi == NULL)
		return;

	if (gi->filterDs != NULL)
	{
		for (int j = 0; j < gi->nphaseA; j++)
		{
			if (gi->filterDs[j] == NULL)
				continue;
			if (gi->filterDsOpen)
				datumstreamread_close_file(gi->filterDs[j]);
			destroy_datumstreamread(gi->filterDs[j]);
			gi->filterDs[j] = NULL;
		}
		gi->filterDsOpen = false;
	}

	if (gi->filterSlot != NULL)
	{
		ExecDropSingleTupleTableSlot(gi->filterSlot);
		gi->filterSlot = NULL;
	}

	if (gi->stageCtx != NULL)
	{
		MemoryContextDelete(gi->stageCtx);
		gi->stageCtx = NULL;
		gi->stages = NULL;
		gi->nstages = 0;
	}

	/* Created by CreateStandaloneExprContext(), so nothing else frees it. */
	if (gi->econtext != NULL)
	{
		FreeExprContext(gi->econtext, true);
		gi->econtext = NULL;
	}

	if (gi->blkdir != NULL)
	{
		AppendOnlyBlockDirectory_End_forSearch(gi->blkdir);
		pfree(gi->blkdir);
		gi->blkdir = NULL;
	}

	if (gi->surv.rownums != NULL)
	{
		pfree(gi->surv.rownums);
		gi->surv.rownums = NULL;
	}
	if (gi->surv.values != NULL)
	{
		pfree(gi->surv.values);
		pfree(gi->surv.isnull);
		gi->surv.values = NULL;
		gi->surv.isnull = NULL;
	}
	gi->surv.nrows = 0;
	gi->surv.capacity = 0;
}

/*
 * Drop the per-segment state so the next segfile starts clean.
 *
 * The survivor array is kept allocated but emptied: sizing it is proportional
 * to a single segfile's hit count, not the whole table, and
 * reusing the allocation avoids repalloc churn across segments.
 */
static void stagedscan_reset_stage_stats(StagedScanState *gi);

/* Close the Phase A streams of the current segment file, if open. */
static void
stagedscan_close_phase_a(StagedScanState *gi)
{
	if (!gi->filterDsOpen)
		return;
	for (int j = 0; j < gi->nphaseA; j++)
		if (gi->filterDs[j] != NULL)
			datumstreamread_close_file(gi->filterDs[j]);
	gi->filterDsOpen = false;
}

void
stagedscan_reset_for_segment(StagedScanState *gi)
{
	gi->surv.nrows = 0;
	gi->cursor = 0;
	gi->phaseADone = false;
	gi->phaseAEof = false;
	gi->phaseABatches = 0;
	gi->batchTarget = STAGEDSCAN_BATCH_FIRST_ROWS;
	gi->rowsScannedSeg = 0;
	gi->rowsInvisibleSeg = 0;
	gi->blocksScannedA = 0;
	gi->blocksHitA = 0;
	gi->curBlockFirstRow = -1;
	gi->curBlockHit = false;

	stagedscan_close_phase_a(gi);

	memset(gi->colCursor, 0, sizeof(StagedScanColumnCursor) * gi->ncols);

	/* the adaptive reordering starts a fresh window with every segfile */
	stagedscan_reset_stage_stats(gi);
	gi->rowsSinceAdapt = 0;
	gi->adaptWindow = STAGED_ADAPT_INITIAL_WINDOW;
}

/* Zero the per-window observations of every Phase B stage. */
static void
stagedscan_reset_stage_stats(StagedScanState *gi)
{
	for (int s = 1; s < gi->nstages; s++)
	{
		gi->stages[s].rowsIn = 0;
		gi->stages[s].rowsRejected = 0;
		gi->stages[s].rowsSettled = 0;
		gi->stages[s].blocksDecompressed = 0;
	}
}

/*
 * Adaptive stage ordering, run at the end of every window of survivors and
 * when a segfile has been fully consumed.
 *
 * The planner orders the Phase B stages from statistics; the data can
 * disagree (a clause estimated selective turns out to reject almost nothing,
 * or its column is far more expensive to fetch than assumed). Each stage
 * records, for the current window, how many rows reached it, how many it
 * rejected and how many varblocks it decompressed for that. The score of a
 * stage is its rejection rate per unit of work, the (s - 1) / c rank
 * ordering of Krishnamurthy et al. with c measured rather than estimated, and
 * adjacent stages whose scores invert the order by more than a hysteresis
 * margin are swapped for the rows that follow, one bubble pass per window as
 * in Babu et al.'s A-Greedy. The Phase A prefix never moves: it has already
 * been read for every row of the segfile and is the part the survivor set is
 * defined by.
 *
 * Windows are counted in survivors entering Phase B, since that is where
 * every cost and every observation of Phase B arises. The first window is
 * STAGED_ADAPT_INITIAL_WINDOW survivors and each later one twice
 * the previous, so a wrong order is corrected after a small prefix of the
 * segfile (most tables have one or two segfiles per segment, so waiting for
 * a segfile boundary would rarely help) while a settled order is re-checked
 * ever more rarely. The counters are zeroed after every check, so each
 * window's scores describe the order actually in force during it: a swap
 * made on the previous window's numbers is confirmed or undone by the next
 * window's own measurement, which is what keeps the independence assumption
 * behind comparing two stages' scores from mattering much.
 *
 * The rejections of an OR clause are all charged to the stage of its last
 * disjunct. An earlier disjunct's worth is the rows it settles, which spares
 * the later disjuncts' fetches but not the projection, so a settled row
 * counts for a fraction of a rejected one: enough to sink a stage that does
 * nothing behind a disjunct that does something. Two adjacent stages that
 * both evaluate disjuncts of the same OR clause are never swapped with each
 * other; the order of the disjuncts within an OR clause stays the planner's.
 *
 * A swap only changes the order in which the same columns are fetched and
 * the same clauses evaluated for a row; stagedscan_build_stages() recomputes
 * which stage settles which clause, so results are unaffected. A swap that
 * would decide a clause able to fail ahead of a clause the qual evaluates
 * before it is not made (stagedscan_order_safe()).
 */
#define STAGED_ADAPT_BLOCK_ROWS		16		/* one decompression ~ this many row fetches */
#define STAGED_ADAPT_HYSTERESIS		1.25	/* the later stage must beat the earlier by this */
#define STAGED_ADAPT_SETTLE_CREDIT	0.25	/* a settled row, in rejected rows */

/* Do stages s and t both evaluate a disjunct of the same OR clause? */
static bool
stagedscan_stages_share_or(const StagedScanState *gi, int s, int t)
{
	const StagedScanStage *a = &gi->stages[s];
	const StagedScanStage *b = &gi->stages[t];

	for (int i = 0; i < a->nevals; i++)
	{
		if (a->evals[i].disjunct < 0)
			continue;
		for (int j = 0; j < b->nevals; j++)
			if (b->evals[j].clause == a->evals[i].clause)
				return true;
	}
	return false;
}

void
stagedscan_adapt_stage_order(StagedScanState *gi, struct AOCSScanDescData *scan)
{
	double	   *score;
	int		   *orig;			/* per position: the stage that was there */
	int		   *clauseStage;
	int			nclauses = list_length(gi->qual);
	bool		changed = false;
	int			k0 = gi->nphaseA;

	if (gi->nstages <= 2)
	{
		stagedscan_reset_stage_stats(gi);
		return;
	}

	score = (double *) palloc(sizeof(double) * gi->nstages);
	orig = (int *) palloc(sizeof(int) * gi->nstages);
	for (int s = 1; s < gi->nstages; s++)
	{
		StagedScanStage *st = &gi->stages[s];
		double		work;

		orig[s] = s;

		/*
		 * A stage that saw too few rows -- because the ones before it were
		 * good -- is left where it is: no swap involving it can be justified.
		 */
		if (st->rowsIn < STAGED_ADAPT_MIN_STAGE_ROWS)
			score[s] = -1.0;
		else
		{
			work = (double) st->rowsIn +
				STAGED_ADAPT_BLOCK_ROWS * (double) st->blocksDecompressed;
			score[s] = ((double) st->rowsRejected +
						STAGED_ADAPT_SETTLE_CREDIT * (double) st->rowsSettled) / work;
		}
	}

	clauseStage = (int *) palloc(sizeof(int) * Max(nclauses, 1));
	for (int s = 1; s + 1 < gi->nstages; s++)
	{
		if (score[s] < 0 || score[s + 1] < 0)
			continue;
		if (stagedscan_stages_share_or(gi, orig[s], orig[s + 1]))
			continue;
		if (score[s + 1] > score[s] * STAGED_ADAPT_HYSTERESIS)
		{
			AttrNumber	tmp = gi->stageAttnos[k0 + s - 1];
			double		tscore = score[s];
			int			torig = orig[s];

			gi->stageAttnos[k0 + s - 1] = gi->stageAttnos[k0 + s];
			gi->stageAttnos[k0 + s] = tmp;

			/*
			 * Not a swap that would move a clause able to fail ahead of one
			 * the qual evaluates before it (stagedscan_order_safe()).
			 */
			stagedscan_assign_clauses(gi->qual, gi->stageAttnos, gi->npos, k0,
									  gi->ncols, clauseStage);
			if (!stagedscan_order_safe(nclauses, clauseStage, gi->clauseCanFail))
			{
				gi->stageAttnos[k0 + s] = gi->stageAttnos[k0 + s - 1];
				gi->stageAttnos[k0 + s - 1] = tmp;
				continue;
			}

			score[s] = score[s + 1];
			score[s + 1] = tscore;
			orig[s] = orig[s + 1];
			orig[s + 1] = torig;
			changed = true;
		}
	}
	pfree(clauseStage);
	pfree(orig);
	pfree(score);

	if (!changed)
	{
		/* the next window is judged on its own numbers */
		stagedscan_reset_stage_stats(gi);
		return;
	}

	/* rebuilding the stages starts them with zeroed counters */
	stagedscan_build_stages(gi, scan);
	gi->stageReorders++;
#ifdef FAULT_INJECTOR
	FaultInjector_InjectFaultIfSet("StagedScan_Stage_Reordered",
								   DDLNotSpecified, "",
								   RelationGetRelationName(scan->rs_base.rs_rd));
#endif
}

/*
 * Open, or reopen for a new segfile, the Phase A stream of one column.
 */
static void
stagedscan_open_phase_a_stream(StagedScanState *gi, AOCSScanDesc scan,
							   AOCSFileSegInfo *segInfo, int j)
{
	AttrNumber	attno = gi->phaseAAttnos[j];

	if (gi->filterDs[j] == NULL)
	{
		gi->filterDs[j] = stagedscan_open_filter_stream(scan, segInfo, attno);
		return;
	}

	/*
	 * Reuse the stream object across segments; only the file changes.
	 * stagedscan_open_filter_stream() would re-read pg_attribute_encoding
	 * every time.
	 */
	stagedscan_open_segfile(scan, segInfo, gi->filterDs[j], attno);
}

/*
 * Hand the scan to the ordinary scan from this segment file on, and say so to
 * EXPLAIN ANALYZE. Phase A's streams are closed now rather than at the end of
 * the scan: nothing reads them again. Returns false, for
 * stagedscan_run_phase_a() to return.
 */
static bool
stagedscan_fall_back(StagedScanState *gi, AOCSScanDesc scan)
{
	gi->fellBack = true;
	if (scan->stagedInstrument != NULL)
		scan->stagedInstrument->staged_fallbacks++;
	stagedscan_close_phase_a(gi);
	return false;
}

/*
 * Is the batch Phase A is filling complete?
 *
 * A batch is complete at its target size (batchTarget: the first batch's
 * STAGEDSCAN_BATCH_FIRST_ROWS, doubling afterwards), and never exceeds
 * STAGEDSCAN_BATCH_MAX_ROWS, the array's bound. The first batch of a
 * single-stage predicate is also the evidence for the two fallback checks
 * below, which need Phase A to have crossed STAGED_DENSITY_MIN_BLOCKS blocks
 * to tell a dense table from a dense band; so that batch keeps filling,
 * whatever its size, until enough blocks have been seen or the array is
 * full. On a sparse predicate those blocks hold few rows and pass in
 * milliseconds; on a dense one they hold the survivors that make the case
 * for falling back, and a full array that arrives first is judged against
 * the planner's estimate instead (see stagedscan_run_phase_a()).
 *
 * The regression tests close a batch on every survivor through the
 * StagedScan_PhaseA_BatchComplete fault: few tables small enough for a test
 * fill a batch by themselves, and every row then crosses a batch
 * boundary, the hardest case for the resume logic.
 */
static inline bool
stagedscan_batch_complete(StagedScanState *gi, AOCSScanDesc scan, bool firstBatch)
{
	if (gi->surv.nrows >= STAGEDSCAN_BATCH_MAX_ROWS)
		return true;
#ifdef FAULT_INJECTOR
	if (gi->surv.nrows > 0 &&
		FaultInjector_InjectFaultIfSet("StagedScan_PhaseA_BatchComplete",
									   DDLNotSpecified, "",
									   RelationGetRelationName(scan->rs_base.rs_rd)) == FaultInjectorTypeSkip)
		return true;
#endif
	if (gi->surv.nrows < gi->batchTarget)
		return false;
	if (firstBatch && (gi->nstages == 1 || gi->phaseASel > 0.0) &&
		gi->blocksScannedA < STAGED_DENSITY_MIN_BLOCKS)
		return false;
	return true;
}

/*
 * Whether the first batch's survivors sit in so many of the anchor column's
 * blocks that Phase B would decompress nearly all of them anyway.
 *
 * The same per-column estimate as the planner's staged_fetch_cost() (see
 * stagedscancost.c), evaluated on what Phase A observed instead of on
 * statistics: h = fraction of blocks holding a survivor, s = survivors per
 * such block, rpb = rows per block. Phase B pays h blocks decompressed, about
 * rpb*s/(s+1) datums advanced in each (datumstreamread_find() steps datum by
 * datum) and a per-block walk overhead charged like a decompression; the
 * ordinary scan pays every block and every datum. The anchor column stands
 * in for the columns Phase B will fetch: their rows per block differ with
 * width, the survivors' spread over the row space does not.
 */
static bool
stagedscan_survivors_too_dense(StagedScanState *gi)
{
	double		h;
	double		s;
	double		rpb;
	double		u = cpu_operator_cost;
	double		dpb = gp_cpu_decompress_cost;
	double		staged;
	double		seq;

	/*
	 * No survivor yet, so Phase B has walked nothing. The walk term below is
	 * charged per block regardless of h, which with about one row per block
	 * (a very wide Phase A column) would call even an empty batch dense.
	 */
	if (gi->blocksHitA == 0)
		return false;

	h = (double) gi->blocksHitA / (double) gi->blocksScannedA;
	s = (double) gi->surv.nrows / (double) gi->blocksHitA;
	rpb = (double) gi->rowsScannedSeg / (double) gi->blocksScannedA;
	staged = h * (dpb + rpb * (s / (s + 1.0)) * u) + dpb;
	seq = dpb + rpb * u;

	return staged >= STAGED_DENSITY_FALLBACK_RATIO * seq;
}

/*
 * The datetime_ops btree family (date, timestamp, timestamptz) has no OID
 * symbol in pg_opfamily.dat; integer_ops has INTEGER_BTREE_FAM_OID.
 */
#define STAGED_DATETIME_BTREE_FAM_OID 434

/*
 * Compile Phase A's clauses into the fast-path form, if they qualify: one
 * Phase A column of a fixed-width integer-like type (int2/int4/int8, date,
 * timestamp, timestamptz), and every clause <Var op Const> or <Const op Var>
 * with a btree comparison operator of that type's family and a non-NULL
 * constant from the same value domain (any integer width for the integer
 * family, exactly the column's type for the datetime family, so that the
 * comparison is a plain int64 comparison). Anything else leaves nsimple at
 * 0 and Phase A evaluates ExecQual() row by row as before.
 */
static void
stagedscan_compile_simple(StagedScanState *gi, List *quals, TupleDesc tupdesc)
{
	AttrNumber	attno;
	Form_pg_attribute att;
	Oid			famoid;
	bool		integerFamily;
	StagedScanSimpleClause *simple;
	int			n = 0;
	ListCell   *lc;

	gi->nsimple = 0;
	if (gi->nphaseA != 1 || quals == NIL)
		return;

	attno = gi->phaseAAttnos[0];
	att = TupleDescAttr(tupdesc, attno);
	if (!att->attbyval)
		return;
	switch (att->atttypid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
			famoid = INTEGER_BTREE_FAM_OID;
			integerFamily = true;
			break;
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			famoid = STAGED_DATETIME_BTREE_FAM_OID;
			integerFamily = false;
			break;
		default:
			return;
	}
	if (att->attlen != 2 && att->attlen != 4 && att->attlen != 8)
		return;

	simple = (StagedScanSimpleClause *)
		palloc(sizeof(StagedScanSimpleClause) * list_length(quals));

	foreach(lc, quals)
	{
		Node	   *clause = (Node *) lfirst(lc);
		OpExpr	   *op;
		Node	   *l,
				   *r;
		Var		   *var;
		Const	   *cst;
		bool		flipped;
		int			strategy;
		int64		value;

		if (!IsA(clause, OpExpr))
			return;
		op = (OpExpr *) clause;
		if (list_length(op->args) != 2)
			return;
		l = (Node *) linitial(op->args);
		r = (Node *) lsecond(op->args);
		while (IsA(l, RelabelType))
			l = (Node *) ((RelabelType *) l)->arg;
		while (IsA(r, RelabelType))
			r = (Node *) ((RelabelType *) r)->arg;
		if (IsA(l, Var) && IsA(r, Const))
		{
			var = (Var *) l;
			cst = (Const *) r;
			flipped = false;
		}
		else if (IsA(l, Const) && IsA(r, Var))
		{
			var = (Var *) r;
			cst = (Const *) l;
			flipped = true;
		}
		else
			return;
		if (var->varattno != attno + 1 || var->varlevelsup != 0 ||
			var->vartype != att->atttypid)
			return;
		if (cst->constisnull)
			return;

		strategy = get_op_opfamily_strategy(op->opno, famoid);
		if (strategy == InvalidStrategy)
			return;
		if (flipped)
		{
			/* c op v  ==  v (mirror op) c */
			switch (strategy)
			{
				case BTLessStrategyNumber:
					strategy = BTGreaterStrategyNumber;
					break;
				case BTLessEqualStrategyNumber:
					strategy = BTGreaterEqualStrategyNumber;
					break;
				case BTGreaterEqualStrategyNumber:
					strategy = BTLessEqualStrategyNumber;
					break;
				case BTGreaterStrategyNumber:
					strategy = BTLessStrategyNumber;
					break;
				default:
					break;
			}
		}

		if (integerFamily)
		{
			switch (cst->consttype)
			{
				case INT2OID:
					value = DatumGetInt16(cst->constvalue);
					break;
				case INT4OID:
					value = DatumGetInt32(cst->constvalue);
					break;
				case INT8OID:
					value = DatumGetInt64(cst->constvalue);
					break;
				default:
					return;
			}
		}
		else
		{
			if (cst->consttype != att->atttypid)
				return;
			value = (att->attlen == 4) ? (int64) DatumGetInt32(cst->constvalue)
				: DatumGetInt64(cst->constvalue);
		}

		simple[n].strategy = strategy;
		simple[n].value = value;
		n++;
	}

	gi->simple = simple;
	gi->nsimple = n;
	gi->simpleLen = att->attlen;
}

/*
 * Can this block of the Phase A column be tested on its raw datum array?
 * Original format lays fixed-width datums out contiguously; a NULL bitmap or
 * the RLE/delta format would need the reader's own walk.
 */
static inline bool
stagedscan_block_is_fast(StagedScanState *gi, DatumStreamRead *ds)
{
	DatumStreamBlockRead *dsr = &ds->blockRead;

	return gi->nsimple > 0 &&
		dsr->datumStreamVersion == DatumStreamVersion_Original &&
		!dsr->has_null &&
		dsr->typeInfo.datumlen == gi->simpleLen &&
		dsr->logical_row_count > 0;
}

static inline bool
stagedscan_simple_match(const StagedScanSimpleClause *simple, int nsimple, int64 v)
{
	for (int k = 0; k < nsimple; k++)
	{
		const int64 c = simple[k].value;
		bool		ok;

		switch (simple[k].strategy)
		{
			case BTLessStrategyNumber:
				ok = v < c;
				break;
			case BTLessEqualStrategyNumber:
				ok = v <= c;
				break;
			case BTEqualStrategyNumber:
				ok = v == c;
				break;
			case BTGreaterEqualStrategyNumber:
				ok = v >= c;
				break;
			default:
				ok = v > c;
				break;
		}
		if (!ok)
			return false;
	}
	return true;
}

/*
 * Account a block Phase A read, as aocs_getnext() accounts the blocks of the
 * scan's own streams: in the bytes the scan read and, as an estimated block
 * count, in the relation's statistics.
 */
static inline void
stagedscan_count_block_read(AOCSScanDesc scan, DatumStreamRead *ds)
{
	scan->totalBytesRead += ds->ao_read.current.isCompressed ?
		ds->ao_read.current.compressedLen : ds->ao_read.current.uncompressedLen;
	pgstat_count_buffer_read_ao(scan->rs_base.rs_rd,
								RelationGuessNumberOfBlocksFromSize(scan->totalBytesRead));
}

/*
 * Phase A fast path: test every row of the block just read against the
 * compiled clauses straight off the raw datum array, appending survivors, and
 * stop early when the batch is complete. Keeps the same counters, visimap
 * check and fault-injector point as the row-at-a-time loop. Returns how many
 * rows of the block were consumed (rows [0, consumed)); the caller positions
 * the stream on the last of them so that the next datumstreamread_advance()
 * continues exactly where a row-at-a-time scan would have.
 */
static int
stagedscan_phase_a_scan_block(StagedScanState *gi, AOCSScanDesc scan,
							  AOCSFileSegInfo *segInfo, DatumStreamRead *ds,
							  bool firstBatch, bool checkVisimap)
{
	DatumStreamBlockRead *dsr = &ds->blockRead;
	const uint8 *base = dsr->datum_beginp;
	const int	len = gi->simpleLen;
	const int	n = dsr->logical_row_count;
	const int64 first = ds->blockFirstRowNum;
	const StagedScanSimpleClause *simple = gi->simple;
	const int	nsimple = gi->nsimple;
	int			i;

	if (first != gi->curBlockFirstRow)
	{
		gi->curBlockFirstRow = first;
		gi->curBlockHit = false;
		gi->blocksScannedA++;
	}
	gi->blocksFastA++;

#ifdef FAULT_INJECTOR
	FaultInjector_InjectFaultIfSet("StagedScan_PhaseA_FastBlock",
								   DDLNotSpecified, "",
								   RelationGetRelationName(scan->rs_base.rs_rd));
#endif

	for (i = 0; i < n; i++)
	{
		int64		v;
		int64		rowNum;

		if (len == 8)
		{
			int64		t;

			memcpy(&t, base + (size_t) i * 8, 8);
			v = t;
		}
		else if (len == 4)
		{
			int32		t;

			memcpy(&t, base + (size_t) i * 4, 4);
			v = t;
		}
		else
		{
			int16		t;

			memcpy(&t, base + (size_t) i * 2, 2);
			v = t;
		}

		gi->rowsScanned++;
		gi->rowsScannedSeg++;

		if (!stagedscan_simple_match(simple, nsimple, v))
			continue;

		rowNum = first + i;
		if (checkVisimap)
		{
			AOTupleId	aoTupleId;

			AOTupleIdInit(&aoTupleId, segInfo->segno, rowNum);
			if (!AppendOnlyVisimap_IsVisible(&scan->visibilityMap, &aoTupleId))
			{
				gi->rowsInvisible++;
				gi->rowsInvisibleSeg++;
				continue;
			}
		}

		/*
		 * A fast block holds no NULLs, and the column is one of the by-value
		 * types the fast path admits, so the datum is the value at its width.
		 */
		if (!stagedscan_surv_append(gi, rowNum,
									len == 8 ? Int64GetDatum(v) :
									len == 4 ? Int32GetDatum((int32) v) :
									Int16GetDatum((int16) v),
									false))
			elog(ERROR, "staged scan: survivor batch overflow");

		gi->rowsSurviving++;
		if (!gi->curBlockHit)
		{
			gi->curBlockHit = true;
			gi->blocksHitA++;
		}

#ifdef FAULT_INJECTOR
		FaultInjector_InjectFaultIfSet("StagedScan_PhaseA_Survivor",
									   DDLNotSpecified, "",
									   RelationGetRelationName(scan->rs_base.rs_rd));
#endif

		if (stagedscan_batch_complete(gi, scan, firstBatch))
		{
			i++;				/* rows [0, i] consumed */
			break;
		}
	}

	return i;
}

/*
 * Phase A: scan the filter column(s) of one segfile, evaluate the predicate,
 * and record the visible surviving row numbers -- one batch at a time.
 *
 * A call fills the survivor array with one batch of survivors -- the first
 * batch holds STAGEDSCAN_BATCH_FIRST_ROWS, each later one twice the previous
 * up to STAGEDSCAN_BATCH_MAX_ROWS -- or runs to the end of the segfile, and
 * returns with
 * gi->phaseADone set; Phase B then consumes the batch and calls again until
 * gi->phaseAEof. The Phase A streams stay open between calls and resume where
 * they stopped, so the segfile is still read exactly once. Batching is what
 * bounds the memory: a survivor array never holds more than one batch,
 * however large the segfile or however many rows match.
 *
 * With several Phase A columns their streams advance in lockstep, one row at
 * a time, the way aocs_getnext() advances the projected columns: each
 * column's varblocks break at different rows, but the row sequence is the
 * same in every column of a segfile.
 *
 * Returns false if Staged Scan must fall back to a normal scan for this
 * query. That can only happen before the first batch is handed over -- a
 * Phase A column has missing values in this segment, or, for a single-stage
 * predicate, the first batch shows the predicate keeping more than
 * whpg_staged_scan_min_selectivity of the rows it scanned, or, for a
 * multi-stage predicate, Phase A keeping several times what the planner
 * estimated -- because the fallback hands the segment to aocs_getnext() as
 * if nothing had been read, which is only true while no row has been
 * emitted.
 */
bool
stagedscan_run_phase_a(StagedScanState *gi, AOCSScanDesc scan,
					   AOCSFileSegInfo *segInfo)
{
	DatumStreamRead *anchor;
	ExprContext *econtext = gi->econtext;
	TupleTableSlot *slot = gi->filterSlot;
	ExprState  *filterQual = gi->phaseAQual;
	bool		isSnapshotAny = (scan->rs_base.rs_snapshot == SnapshotAny);
	bool		checkVisimap;
	bool		visibleFirst;
	bool		enoughEvidence;
	bool		fullBatchEvidence;
	int64		segRows;
	MemoryContext oldctx;
	bool		firstBatch = (gi->phaseABatches == 0);

	Assert(!gi->phaseADone);
	Assert(!gi->phaseAEof);
	Assert(filterQual != NULL);

	if (firstBatch)
	{
		/*
		 * If a Phase A column was added by ALTER TABLE ADD COLUMN and is
		 * missing for the older rows of this segment, its values are not in
		 * the segfile at all. Rather than reconstruct them, decline Staged
		 * Scan.
		 */
		for (int j = 0; j < gi->nphaseA; j++)
		{
			/*
			 * The column's segfile holds values only for the rows written
			 * after the ALTER; if even the first row is missing, Phase A
			 * would walk that file and never meet the older rows at all.
			 */
			if (AO_ATTR_VAL_IS_MISSING(1,
									   gi->phaseAAttnos[j],
									   segInfo->segno,
									   scan->columnScanInfo.attnum_to_rownum))
				return stagedscan_fall_back(gi, scan);
		}

		/*
		 * Blocks written before 4.0 carry no first row number, and Phase B
		 * would place rows by header row counts it cannot check against one.
		 * Such blocks can only precede newer ones in a segment file, so the
		 * first block of each projected column tells; the streams have just
		 * read it (stagedscan_seed_cursors()).
		 */
		for (int i = 0; i < scan->columnScanInfo.num_proj_atts; i++)
		{
			AttrNumber	attno = scan->columnScanInfo.proj_atts[i];

			if (scan->columnScanInfo.ds[attno]->getBlockInfo.firstRow < 0)
				return stagedscan_fall_back(gi, scan);
		}

		Assert(!gi->filterDsOpen);
		oldctx = MemoryContextSwitchTo(gi->ctx);
		for (int j = 0; j < gi->nphaseA; j++)
			stagedscan_open_phase_a_stream(gi, scan, segInfo, j);
		MemoryContextSwitchTo(oldctx);
		gi->filterDsOpen = true;

		/* deleted rows of this segfile, from the visimap: once per segfile */
		gi->segHidden = isSnapshotAny ? 0 :
			AppendOnlyVisimap_GetSegmentFileHiddenTupleCount(&scan->visibilityMap,
															 segInfo->segno);
	}
	anchor = gi->filterDs[0];

	/*
	 * A segfile with no deleted rows has no visimap entries; the fast path
	 * skips the per-survivor lookup for it.
	 */
	checkVisimap = gi->segHidden > 0;
	visibleFirst = checkVisimap && gi->phaseAMayFail;

	/* a fresh batch */
	gi->surv.nrows = 0;
	gi->cursor = 0;

	/* Evaluate the qual in the per-tuple context so it gets reset per row. */
	oldctx = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	while (!stagedscan_batch_complete(gi, scan, firstBatch))
	{
		int64		rowNum;
		int			nth;
		bool		eof = false;
		bool		fastBlock = false;
		AOTupleId	aoTupleId;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Advance every Phase A column to the next row and put its value in
		 * the slot. Stage 0's clauses reference these columns alone, so the
		 * rest of the slot is never read.
		 */
		for (int j = 0; j < gi->nphaseA && !eof; j++)
		{
			DatumStreamRead *ds = gi->filterDs[j];
			AttrNumber	attno = gi->phaseAAttnos[j];

			if (datumstreamread_advance(ds) == 0)
			{
				/*
				 * Phase A wants every row of its columns, so it reads header
				 * and content together. It is Phase B that skips.
				 */
				if (datumstreamread_block(ds, NULL, attno) < 0)
				{
					/* Every column of a segfile holds the same rows. */
					if (j > 0)
						elog(ERROR, "staged scan: column %d of table \"%s\" ended before column %d in segment %d",
							 attno + 1,
							 RelationGetRelationName(scan->rs_base.rs_rd),
							 gi->phaseAAttnos[0] + 1, segInfo->segno);
					eof = true;
					break;
				}
				stagedscan_count_block_read(scan, ds);

				/*
				 * A fresh block of the only Phase A column: test it on its
				 * raw datum array when the predicate and the block allow,
				 * then leave the stream on the last row consumed so the
				 * loop resumes as if it had advanced row by row.
				 */
				if (j == 0 && stagedscan_block_is_fast(gi, ds))
				{
					int			consumed;

					consumed = stagedscan_phase_a_scan_block(gi, scan, segInfo, ds,
															 firstBatch, checkVisimap);
					if (consumed > 0)
					{
						datumstreamread_position_fixedwidth(ds, consumed - 1);
						fastBlock = true;
						break;
					}
				}

				if (datumstreamread_advance(ds) == 0)
				{
					if (j > 0)
						elog(ERROR, "staged scan: column %d of table \"%s\" ended before column %d in segment %d",
							 attno + 1,
							 RelationGetRelationName(scan->rs_base.rs_rd),
							 gi->phaseAAttnos[0] + 1, segInfo->segno);
					eof = true;
					break;
				}
			}

			datumstreamread_get(ds, &slot->tts_values[attno],
								&slot->tts_isnull[attno]);
		}
		if (fastBlock)
		{
			fastBlock = false;
			continue;			/* the batch check at the loop head decides */
		}
		if (eof)
		{
			gi->phaseAEof = true;
			break;				/* end of this segfile */
		}

		nth = datumstreamread_nth(anchor);
		Assert(anchor->blockFirstRowNum > 0 && nth >= 0);
		rowNum = anchor->blockFirstRowNum + nth;

		if (anchor->blockFirstRowNum != gi->curBlockFirstRow)
		{
			gi->curBlockFirstRow = anchor->blockFirstRowNum;
			gi->curBlockHit = false;
			gi->blocksScannedA++;
		}

		gi->rowsScanned++;
		gi->rowsScannedSeg++;

		econtext->ecxt_scantuple = slot;
		ResetExprContext(econtext);

		/*
		 * The visimap is normally consulted only for the rows the predicate
		 * keeps, which is cheaper, so that Phase B never materializes a
		 * deleted row. A predicate that can fail on some row must not be
		 * shown a deleted one at all: the ordinary scan never would, and a
		 * row deleted for holding bad data is just the row it fails on.
		 */
		AOTupleIdInit(&aoTupleId, segInfo->segno, rowNum);
		if (visibleFirst &&
			!AppendOnlyVisimap_IsVisible(&scan->visibilityMap, &aoTupleId))
		{
			gi->rowsInvisible++;
			gi->rowsInvisibleSeg++;
			continue;
		}

		if (!ExecQual(filterQual, econtext))
			continue;

		if (!visibleFirst && !isSnapshotAny &&
			!AppendOnlyVisimap_IsVisible(&scan->visibilityMap, &aoTupleId))
		{
			gi->rowsInvisible++;
			gi->rowsInvisibleSeg++;
			continue;
		}

		if (!stagedscan_surv_append(gi, rowNum,
									gi->carryAttno >= 0 ?
									slot->tts_values[gi->carryAttno] : (Datum) 0,
									gi->carryAttno >= 0 ?
									slot->tts_isnull[gi->carryAttno] : true))
			elog(ERROR, "staged scan: survivor batch overflow");

		gi->rowsSurviving++;
		if (!gi->curBlockHit)
		{
			gi->curBlockHit = true;
			gi->blocksHitA++;
		}

#ifdef FAULT_INJECTOR
		FaultInjector_InjectFaultIfSet("StagedScan_PhaseA_Survivor",
									   DDLNotSpecified, "",
									   RelationGetRelationName(scan->rs_base.rs_rd));
#endif
	}

	MemoryContextSwitchTo(oldctx);

	/*
	 * The first batch normally keeps filling until STAGED_DENSITY_MIN_BLOCKS
	 * blocks have passed (stagedscan_batch_complete()), but the array bound
	 * STAGEDSCAN_BATCH_MAX_ROWS can close it sooner; a handful of rows from
	 * the first few blocks is no evidence to fall back on, unless they are
	 * the whole segment file.
	 *
	 * A full array is no handful, but the rate over those few blocks cannot
	 * tell a clustered band at the start of the file (older data in a table
	 * loaded in time order, say) from a predicate that keeps nearly every
	 * row: both keep almost all of what was scanned. Their count can. A band
	 * is what the statistics predicted, while a batch holding more than
	 * STAGED_ESTIMATE_SLACK times the planner's estimate for the whole
	 * segment file has proved the estimate wrong, wherever the rest of the
	 * matches lie, and the band argument rests on that estimate. Such a batch
	 * is evidence enough.
	 */
	segRows = segInfo->total_tupcount - gi->segHidden;
	fullBatchEvidence = gi->surv.nrows >= STAGEDSCAN_BATCH_MAX_ROWS &&
		gi->phaseASel > 0.0 &&
		(double) gi->surv.nrows >
		STAGED_ESTIMATE_SLACK * gi->phaseASel * (double) segRows;
	enoughEvidence = gi->phaseAEof ||
		gi->blocksScannedA >= STAGED_DENSITY_MIN_BLOCKS ||
		fullBatchEvidence;

	/*
	 * Staged Scan is only worth it when the predicate discards most of the
	 * rows. For a single-stage predicate the planner's selectivity gate is
	 * re-checked here on the first batch, before any row has been emitted:
	 * if the rows scanned so far kept more than
	 * whpg_staged_scan_min_selectivity of themselves, the statistics the
	 * planner trusted were wrong and the ordinary scan is the better deal.
	 * With more than one stage the gate applies to the whole predicate,
	 * which the planner has already checked; Phase A alone may legitimately
	 * keep far more for the later stages to whittle down. Later batches
	 * never fall back: rows have been emitted, and the scan stays correct
	 * either way, only less profitable.
	 */
	if (firstBatch && gi->nstages == 1 && gi->rowsScannedSeg > 0 &&
		enoughEvidence &&
		(double) gi->surv.nrows >
		(double) gi->rowsScannedSeg * whpg_staged_scan_min_selectivity)
		return stagedscan_fall_back(gi, scan);

	/*
	 * For a multi-stage predicate Phase A may legitimately keep far more than
	 * the gate, but not far more than the planner thought it would: the
	 * stage order and the whole decision rest on that estimate, and a clause
	 * on an expression the statistics know nothing about is estimated at
	 * 0.5% whether it keeps 0.5% or 50%. The first batch keeps filling until
	 * STAGED_DENSITY_MIN_BLOCKS blocks have passed when this check is armed,
	 * so a band at the start of the file does not look like a dense table.
	 * Fall back when the observed rate is both several times the estimate
	 * and well above the gate; smaller misses are the later stages' and the
	 * run-time reordering's business.
	 */
	if (firstBatch && gi->nstages > 1 && gi->phaseASel > 0.0 &&
		gi->rowsScannedSeg > 0 && enoughEvidence)
	{
		double		observed = (double) gi->surv.nrows / (double) gi->rowsScannedSeg;

		if (observed > STAGED_ESTIMATE_SLACK * gi->phaseASel &&
			observed > STAGED_ESTIMATE_MIN_GATE_MULT * whpg_staged_scan_min_selectivity)
		{
#ifdef FAULT_INJECTOR
			FaultInjector_InjectFaultIfSet("StagedScan_PhaseA_EstimateFallback",
										   DDLNotSpecified, "",
										   RelationGetRelationName(scan->rs_base.rs_rd));
#endif
			return stagedscan_fall_back(gi, scan);
		}
	}

	/*
	 * The other first-batch, single-stage check: are the survivors clustered
	 * enough for Phase B to skip anything? The planner may have accepted this
	 * scan on the hope that they are (its correlation statistic cannot see a
	 * table loaded in parallel, whose interleaved ascending runs are clustered
	 * in blocks but not in correlation); the batch is the evidence. Judged
	 * only once Phase A has crossed enough blocks: a batch that fills within
	 * a few blocks is a clustered band, dense inside itself and empty around
	 * it, which is exactly the shape Staged Scan is for -- unless it holds
	 * far more than the statistics allow the whole file (fullBatchEvidence).
	 */
	if (firstBatch && gi->nstages == 1 &&
		(gi->blocksScannedA >= STAGED_DENSITY_MIN_BLOCKS || fullBatchEvidence) &&
		stagedscan_survivors_too_dense(gi))
	{
#ifdef FAULT_INJECTOR
		FaultInjector_InjectFaultIfSet("StagedScan_PhaseA_DenseFallback",
									   DDLNotSpecified, "",
									   RelationGetRelationName(scan->rs_base.rs_rd));
#endif
		return stagedscan_fall_back(gi, scan);
	}

	gi->phaseABatches++;
	gi->phaseADone = true;
	gi->batchTarget = Min(gi->batchTarget * 2, (int64) STAGEDSCAN_BATCH_MAX_ROWS);

	/*
	 * At the end of a segfile, settle the deleted rows that failed the
	 * predicate: the visimap knows how many rows of the segfile are hidden,
	 * and those that passed the predicate were counted in rowsInvisibleSeg
	 * as Phase A met them; the rest were rejected by the predicate before
	 * anyone asked whether they were deleted. An ordinary scan never shows
	 * a deleted row to its filter, so they must not count as removed by it.
	 */
	if (gi->phaseAEof && gi->segHidden > gi->rowsInvisibleSeg)
		gi->rowsHiddenRejected += gi->segHidden - gi->rowsInvisibleSeg;

	/* hand the running totals to EXPLAIN ANALYZE (see nodeSeqscan.c) */
	if (scan->stagedInstrument != NULL)
	{
		scan->stagedInstrument->staged_rows_scanned = gi->rowsScanned;
		scan->stagedInstrument->staged_survivors = gi->rowsSurviving;
		/*
		 * What the predicate rejected so far: Phase A's rejections, less the
		 * deleted rows among them, plus the later stages'. EXPLAIN ANALYZE
		 * folds this into "Rows Removed by Filter", so the count stays what
		 * a user expects of a filter, whichever phase did the rejecting.
		 */
		scan->stagedInstrument->staged_rows_removed =
			(gi->rowsScanned - gi->rowsInvisible - gi->rowsSurviving -
			 gi->rowsHiddenRejected) +
			gi->rowsRejectedLater;
	}

#ifdef FAULT_INJECTOR
	FaultInjector_InjectFaultIfSet("StagedScan_PhaseA_Batch",
								   DDLNotSpecified, "",
								   RelationGetRelationName(scan->rs_base.rs_rd));
#endif

	if (gi->phaseAEof)
	{
		for (int j = 0; j < gi->nphaseA; j++)
			datumstreamread_close_file(gi->filterDs[j]);
		gi->filterDsOpen = false;

#ifdef FAULT_INJECTOR
		FaultInjector_InjectFaultIfSet("StagedScan_PhaseA_Complete",
									   DDLNotSpecified, "",
									   RelationGetRelationName(scan->rs_base.rs_rd));
#endif
	}

	return true;
}

/*
 * Decide whether Staged Scan applies to this scan, given the stage list the
 * planner put on the plan.
 *
 * Staged Scan only pays off when there is something left to fetch after
 * Phase A: a later stage, or a projected column other than Phase A's. If the
 * qual and the target list cover the same single column, ordinary AOCO column
 * projection already avoids all the work Staged Scan would save.
 *
 * 'proj' is the projection mask computed by the caller (may be NULL, meaning
 * "no column could be determined", in which case Staged Scan does not apply).
 */
bool
stagedscan_applicable(Relation rel, List *targetlist, List *qual,
					  bool *proj, AttrNumber natts, List *filterAttnos,
					  int phaseAWidth)
{
	bool	   *qualcols;
	AttrNumber	stageAttnos[MaxHeapAttributeNumber];
	int			nstages;
	int		   *clauseStage;
	bool	   *canFail;
	bool		ok;

	if (!whpg_enable_staged_scan)
		return false;

	if (qual == NIL || proj == NULL || filterAttnos == NIL)
		return false;
	if (phaseAWidth < 1 || phaseAWidth > list_length(filterAttnos))
		return false;

	/*
	 * Staged Scan is AOCO-only: on AO_ROW every column shares a varblock, so
	 * skipping one column's blocks saves nothing. A partition of another
	 * access method under a dynamic scan reaches here too.
	 */
	if (!RelationIsAoCols(rel))
		return false;

	/*
	 * The predicate has to be safe to evaluate out of order and
	 * once per row, ahead of the rest of the tuple being assembled.
	 */
	if (contain_volatile_functions((Node *) qual) ||
		contain_subplans((Node *) qual) ||
		stagedscan_contains_param((Node *) qual))
		return false;

	/* System columns and whole-row references live in no column file. */
	if (stagedscan_contains_sysattr((Node *) qual))
		return false;

	/*
	 * Row-level security needs nothing of its own here: order_qual_clauses()
	 * puts a policy's clauses ahead of the user's leaky ones, and a leaky
	 * clause can fail, so stagedscan_order_safe() below keeps it there.
	 * (cost_aocs_stagedscan() also declines a predicate mixing security
	 * levels with a leaky clause outright.)
	 */

	nstages = stagedscan_stage_array(filterAttnos, RelationGetDescr(rel),
									 stageAttnos, lengthof(stageAttnos));
	if (nstages <= 0)
		return false;

	/*
	 * The stage columns and the columns the predicate reads must be the same
	 * set: a stage column the predicate never reads would be a stage with
	 * nothing to decide, and a predicate column that is no stage would be a
	 * clause no stage decides -- which no one else enforces, since the scan
	 * node runs no qual of its own on a staged scan.
	 */
	qualcols = (bool *) palloc0(sizeof(bool) * natts);
	ok = extractcolumns_from_node((Node *) qual, qualcols, natts);
	for (int s = 0; ok && s < nstages; s++)
		if (!qualcols[stageAttnos[s]])
			ok = false;
	for (AttrNumber a = 0; ok && a < natts; a++)
	{
		int			s;

		if (!qualcols[a])
			continue;
		for (s = 0; s < nstages; s++)
			if (stageAttnos[s] == a)
				break;
		if (s == nstages)
			ok = false;
	}
	pfree(qualcols);
	if (!ok)
		return false;

	/*
	 * Phase A with no clause would keep every row; refuse that. And refuse
	 * an order that would show a clause able to fail a row the ordinary scan
	 * would not (stagedscan_order_safe()): whoever built the plan, the scan
	 * then raises only the errors the plan without it would raise.
	 */
	clauseStage = (int *) palloc(sizeof(int) * list_length(qual));
	canFail = (bool *) palloc(sizeof(bool) * list_length(qual));
	ok = stagedscan_assign_clauses(qual, stageAttnos, nstages, phaseAWidth,
								   natts, clauseStage) > 0;
	if (ok)
	{
		int			i = 0;
		ListCell   *lc;

		foreach(lc, qual)
			canFail[i++] = stagedscan_clause_can_fail((Node *) lfirst(lc));
		ok = stagedscan_order_safe(list_length(qual), clauseStage, canFail);
	}
	pfree(canFail);
	pfree(clauseStage);
	if (!ok)
		return false;

	return true;
}
