/*-------------------------------------------------------------------------
 *
 * planrtfilter.c
 *	  Give a hash join's probe scan a runtime filter over the join keys.
 *
 * A pass over the finished plan, for either optimizer. For every hash join
 * whose probe rows come from a plain scan of a base relation in the same
 * slice -- the join's outer input, looked for through Result nodes and down
 * the outer side of joins that hand the probe row's columns on unchanged,
 * stopping at a Motion -- and whose outer hash key is a column of that scan,
 * the pass:
 *
 *	 - allocates an exec Param, which the join sets to the filter once its
 *	   hash table is built (nodeHashjoin.c) and before it reads a single
 *	   probe row;
 *	 - puts a RuntimeFilterExpr over that Param, the column and the join's
 *	   hash operator after the scan's own clauses (runtimefilter.c), so the
 *	   scan drops rows whose key cannot be in the hash table;
 *	 - on an AOCO table, decides the Staged Scan again with that clause in
 *	   the predicate, so that the filter column can lead Phase A and the
 *	   rows the filter drops never have their other columns decompressed.
 *
 * Only joins that drop probe rows without a match get one (inner, semi,
 * right, and the dedup-semi variants), only equality hash clauses (an IS
 * NOT DISTINCT FROM join keeps NULLs), and no parallel scans or joins (the
 * Param would have to reach the workers). The filter is planned when the
 * probe is large enough, the planner expects few enough of its rows to find
 * a match, and the inner side is small enough for a filter within
 * whpg_runtime_filter_max_size to stay useful.
 *
 * The filter is over one hash key. A join on several keys gets it over the
 * key expected to let the fewest probe rows through -- a probe row passes a
 * filter over one key when that key alone has a match, however the others
 * fare -- estimated per key from the statistics (rf_key_match_fraction()).
 *
 * The estimate of the rows that survive is the join's estimated output over
 * the probe's rows: exact when every probe row matches at most one inner
 * row, and an overestimate of the survivors -- so a cautious one --
 * otherwise; for a filter over one key of several, that key's share if it is
 * larger. It is carried in the clause for the cost models.
 *
 * The pass runs over the plan in its final form, after set_plan_references()
 * for the Postgres planner and over GPORCA's plan as it comes: one range
 * table for the plan and its subplans, the hash keys as references into the
 * children's target lists. The Param is set and read within one slice and
 * nothing rescans by it, so the plan needs no extParam entry for it; the
 * pass therefore also runs after SS_finalize_plan(), which would otherwise
 * take the scan's reference for one nothing supplies.
 *
 * Portions Copyright (c) 2026, WarehousePG contributors.
 *
 * IDENTIFICATION
 *	    src/backend/optimizer/plan/planrtfilter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/gp_distribution_policy.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "cdb/cdbutil.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/walkers.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/runtimefilter.h"
#include "utils/syscache.h"

typedef struct RuntimeFilterPlanContext
{
	plan_tree_base_prefix base;	/* required by plan_tree_walker() */
	PlannerGlobal *glob;
	List	   *rtable;			/* what the plan's scanrelids index */
	List	  **paramExecTypes;	/* where a new Param is declared */
	bool		orcaRows;		/* the row estimates are GPORCA's (rf_plan_rows()) */
	int			sliceIndex;		/* the slice the node being walked runs in, -1 unknown */
} RuntimeFilterPlanContext;

/*
 * Joins that never emit a probe-side row without a match. LEFT, FULL, ANTI
 * and LASJ_NOTIN need the rows the filter would drop.
 */
static bool
rf_join_type_ok(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
		case JOIN_RIGHT:
		case JOIN_DEDUP_SEMI:
		case JOIN_DEDUP_SEMI_REVERSE:
			return true;
		default:
			return false;
	}
}

/*
 * Joins the search for the probe scan may pass through on its way down the
 * outer side. A row the filter drops before such a join takes only its own
 * descendants with it: every row the join derives from it carries the key
 * column as it was, so the upper join would have dropped them all. A RIGHT
 * or FULL join is different: an inner row matched by nothing else would be
 * emitted null-extended in the dropped row's place, a row the plan without
 * the filter never produced. The dedup-semi variants decide which of two
 * duplicates to keep by a row identity, and a dropped row could change that
 * choice; they are left out too.
 */
static bool
rf_lower_join_ok(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_SEMI:
		case JOIN_ANTI:
		case JOIN_LASJ_NOTIN:
			return true;
		default:
			return false;
	}
}

/*
 * Express 'key' in terms of the input of 'plan': a Var referring to plan's
 * output (OUTER_VAR, as GPORCA's plans and set_plan_references() write
 * them) is replaced by the target list entry it names. Vars of a base
 * relation (as the Postgres planner's plans have them before
 * set_plan_references()) are left alone. Returns NULL for any other shape.
 */
static Node *
rf_through_targetlist(Node *key, Plan *plan)
{
	if (key == NULL)
		return NULL;

	if (IsA(key, RelabelType))
	{
		RelabelType *rt = (RelabelType *) key;
		Node	   *arg = rf_through_targetlist((Node *) rt->arg, plan);

		if (arg == NULL)
			return NULL;
		if (arg == (Node *) rt->arg)
			return key;
		rt = copyObject(rt);
		rt->arg = (Expr *) arg;
		return (Node *) rt;
	}

	if (IsA(key, Var))
	{
		Var		   *var = (Var *) key;

		if (var->varlevelsup != 0)
			return NULL;
		if (var->varno == OUTER_VAR)
		{
			/*
			 * The attribute number names the entry's resno, not its
			 * position: the Postgres planner keeps a target list in resno
			 * order, GPORCA's translator need not.
			 */
			TargetEntry *te = get_tle_by_resno(plan->targetlist, var->varattno);

			if (te == NULL)
				return NULL;
			return (Node *) te->expr;
		}
		if (var->varno == INNER_VAR || var->varno == INDEX_VAR)
			return NULL;
		return key;
	}

	return NULL;
}

/*
 * What the search for the probe scan found. A Motion is told apart from the
 * other stops because it is where the cross-segment phase of this feature
 * takes over: the probe scan on the far side of a Redistribute Motion can
 * still use the filter, if it is brought there (see README.runtime_filter.md,
 * "Toward cross-segment filters"); the other stops mean no filter at all.
 */
typedef enum RfProbeSearch
{
	RF_PROBE_FOUND,				/* *scan is the probe scan, in this slice */
	RF_PROBE_MOTION,			/* the probe rows come from another slice */
	RF_PROBE_UNSUPPORTED		/* a join, an Append, an expression key, ... */
} RfProbeSearch;

/*
 * Find the scan the probe rows come from: the join's outer input, through
 * nodes that pass every row on unchanged and keep none, within the slice.
 * 'key' is the outer hash key as the join sees it; when found, *scanKey is
 * that key as the scan sees it, a column of the scanned relation.
 *
 * A Result passes rows on. So does the outer side of a join of the kinds
 * rf_lower_join_ok() admits, as far as the key column is concerned: a star
 * join is a chain of hash joins over one fact scan, and every dimension's
 * filter can be tested at that scan, not only the lowest join's. The key
 * must come down the outer side the whole way; a key from a lower join's
 * inner side stops the search (rf_through_targetlist() hands back an
 * INNER_VAR, or a base Var of another relation than the scan's).
 *
 * A Material does not qualify, although it changes no row: it keeps them.
 * Rescanned with its child unchanged, it replays its tuplestore, and if the
 * hash table had been rebuilt with other keys in between -- the inner side
 * parameterized by an outer loop -- the rows the previous filter dropped
 * would never be read again. Rows a filter drops must come from a scan that
 * is re-read on every rescan. A join re-reads its outer input on rescan, so
 * it is safe to pass; a Sort is not, for the same reason as a Material.
 */
static RfProbeSearch
rf_find_probe_scan(Plan *outer, Node *key, SeqScan **scan, Node **scanKey,
				   bool *dynamic)
{
	Plan	   *cur = outer;

	*scan = NULL;
	for (;;)
	{
		key = rf_through_targetlist(key, cur);
		if (key == NULL)
			return RF_PROBE_UNSUPPORTED;

		if (IsA(cur, SeqScan) || IsA(cur, DynamicSeqScan))
		{
			SeqScan    *found = (SeqScan *) cur;
			Node	   *bare = key;

			while (IsA(bare, RelabelType))
				bare = (Node *) ((RelabelType *) bare)->arg;
			if (!IsA(bare, Var) ||
				((Var *) bare)->varno != found->scan.scanrelid ||
				((Var *) bare)->varattno <= 0)
				return RF_PROBE_UNSUPPORTED;

			/*
			 * A relabel to a polymorphic type names no type to hash by:
			 * GPORCA writes an array key as (karr)::anyarray, and the
			 * probe looks its hash function up by the key's type
			 * (runtimefilter_probe_init()). The column's own type is what
			 * the join hashes; the key is taken down to it.
			 */
			while (IsA(key, RelabelType) &&
				   IsPolymorphicType(((RelabelType *) key)->resulttype))
				key = (Node *) ((RelabelType *) key)->arg;

			*scan = found;
			*scanKey = copyObject(key);
			*dynamic = IsA(cur, DynamicSeqScan);
			return RF_PROBE_FOUND;
		}

		/* 'key' now names cur's input; one level down */
		if (IsA(cur, Result) && outerPlan(cur) != NULL)
		{
			cur = outerPlan(cur);
			continue;
		}

		if ((IsA(cur, HashJoin) || IsA(cur, NestLoop) || IsA(cur, MergeJoin)) &&
			outerPlan(cur) != NULL &&
			rf_lower_join_ok(((Join *) cur)->jointype))
		{
			cur = outerPlan(cur);
			continue;
		}

		if (IsA(cur, Motion))
			return RF_PROBE_MOTION;

		/* a Material, a right or full join, an Append, a Sort, a Limit, ... */
		return RF_PROBE_UNSUPPORTED;
	}
}

/* The survivor estimate a filter clause carries. */
static double
rf_clause_selectivity(Node *clause)
{
	Assert(runtimefilter_is_clause(clause));
	return ((RuntimeFilterExpr *) clause)->selectivity;
}

/*
 * Put a filter clause into the scan's qual: after the scan's own clauses --
 * they are already in cost order and cheaper than a hash and a probe, so
 * the filter sees only the rows they pass -- and among the filter clauses
 * already there (a star join brings the scan one per dimension) in order
 * of their survivor estimates, the most selective first. For a Staged Scan
 * the position is immaterial: the cost model places the clauses, and a
 * filter clause cannot fail (stagedscan_clause_can_fail()), so it is free
 * to move.
 */
static void
rf_add_clause(SeqScan *scan, Expr *clause, double sel)
{
	List	   *filters;
	List	   *rest;
	List	   *ordered = NIL;
	bool		placed = false;
	ListCell   *lc;

	runtimefilter_split_qual(scan->scan.plan.qual, &filters, &rest);
	foreach(lc, filters)
	{
		Node	   *other = (Node *) lfirst(lc);

		if (!placed && sel < rf_clause_selectivity(other))
		{
			ordered = lappend(ordered, clause);
			placed = true;
		}
		ordered = lappend(ordered, other);
	}
	if (!placed)
		ordered = lappend(ordered, clause);
	scan->scan.plan.qual = list_concat(rest, ordered);
}

/*
 * The number of distinct values of a column by its statistics, 0 when it has
 * none. A partitioned table's root carries its partitions' statistics as
 * inherited ones (GPORCA's DynamicSeqScan names the root), a plain table its
 * own; a negative stadistinct is a share of the rows. 'rel' is open.
 */
static double
rf_column_ndistinct(Relation rel, AttrNumber attnum)
{
	Oid			relid = RelationGetRelid(rel);
	bool		inh = (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	HeapTuple	tup;
	float4		stadistinct;

	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(relid),
						  Int16GetDatum(attnum), BoolGetDatum(inh));
	if (!HeapTupleIsValid(tup))
		tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(relid),
							  Int16GetDatum(attnum), BoolGetDatum(!inh));
	if (!HeapTupleIsValid(tup))
		return 0;
	stadistinct = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	ReleaseSysCache(tup);
	if (stadistinct >= 0)
		return stadistinct;
	return -stadistinct * (inh ? cdb_estimate_partitioned_numtuples(rel)
						   : rel->rd_rel->reltuples);
}

/*
 * The base column 'key' names, looked for down the plan from 'plan' through
 * the target lists its Vars refer to (OUTER_VAR and INNER_VAR, as the final
 * form of a plan writes them) to a Var of a range table relation, which a
 * scan's own Vars are. False for any other shape: an expression, a
 * subquery's or an Append's column.
 */
static bool
rf_trace_column(Node *key, Plan *plan, List *rtable,
				Oid *relid, AttrNumber *attnum)
{
	for (;;)
	{
		Var		   *var;
		Plan	   *child;
		TargetEntry *te;
		RangeTblEntry *rte;

		while (key != NULL && IsA(key, RelabelType))
			key = (Node *) ((RelabelType *) key)->arg;
		if (key == NULL || !IsA(key, Var))
			return false;
		var = (Var *) key;
		if (var->varlevelsup != 0 || var->varattno <= 0 ||
			var->varno == INDEX_VAR)
			return false;

		if (var->varno != OUTER_VAR && var->varno != INNER_VAR)
		{
			if (var->varno <= 0 || var->varno > list_length(rtable))
				return false;
			rte = rt_fetch(var->varno, rtable);
			if (rte->rtekind != RTE_RELATION)
				return false;
			*relid = rte->relid;
			*attnum = var->varattno;
			return true;
		}

		/* the Var names an entry of the child's target list */
		if (plan == NULL)
			return false;
		child = (var->varno == OUTER_VAR) ? outerPlan(plan) : innerPlan(plan);
		if (child == NULL)
			return false;
		te = get_tle_by_resno(child->targetlist, var->varattno);
		if (te == NULL)
			return false;
		key = (Node *) te->expr;
		plan = child;
	}
}

/*
 * For one key of a join on several: the share of the probe rows whose value
 * of that key the inner side holds -- what a filter over that key alone
 * would let through. It is the inner side's distinct values of the key over
 * the probe column's, the inner side's keys being taken for a subset of the
 * probe's, as a foreign key's are. The inner side's distinct values are at
 * most its rows, and at most the distinct values of the column they come
 * from, when they are a column's.
 *
 * Rows in a plan are a segment's, and the filter holds a segment's inner
 * rows. The share of the probe column's values they cover is the same
 * whether a segment holds the whole inner side (a Broadcast) or its hash
 * range of it -- except when that range is the probe table's own
 * distribution on this key, for then the segment's probe rows are that
 * range too, and the inner side's rows over all segments stand against the
 * probe column's distinct values.
 *
 * False when the probe column has no statistics to estimate by.
 */
static bool
rf_key_match_fraction(RuntimeFilterPlanContext *cx, HashJoin *hj, int i,
					  SeqScan *scan, Node *scanKey, double probeRows,
					  double buildRows, double *fraction)
{
	Node	   *probeKey = scanKey;
	Var		   *probeVar;
	RangeTblEntry *rte;
	Relation	probeRel;
	GpPolicy   *policy;
	OpExpr	   *hashclause;
	Oid			innerRelid;
	AttrNumber	innerAttno;
	double		nseg = getgpsegmentCount();
	double		probeDistinct;
	double		innerKeys;

	while (IsA(probeKey, RelabelType))
		probeKey = (Node *) ((RelabelType *) probeKey)->arg;
	Assert(IsA(probeKey, Var));
	probeVar = (Var *) probeKey;

	if (scan->scan.scanrelid <= 0 ||
		scan->scan.scanrelid > list_length(cx->rtable))
		return false;
	rte = rt_fetch(scan->scan.scanrelid, cx->rtable);
	if (rte->rtekind != RTE_RELATION)
		return false;

	probeRel = table_open(rte->relid, NoLock);	/* the query holds its lock */
	probeDistinct = rf_column_ndistinct(probeRel, probeVar->varattno);
	policy = probeRel->rd_cdbpolicy;
	if (policy != NULL && policy->ptype == POLICYTYPE_PARTITIONED &&
		policy->nattrs == 1 && policy->attrs[0] == probeVar->varattno)
		buildRows *= nseg;
	table_close(probeRel, NoLock);
	if (probeDistinct <= 0)
		return false;
	probeDistinct = Min(probeDistinct, probeRows * nseg);

	innerKeys = buildRows;
	hashclause = (OpExpr *) list_nth(hj->hashclauses, i);
	if (IsA(hashclause, OpExpr) && list_length(hashclause->args) == 2 &&
		rf_trace_column((Node *) lsecond(hashclause->args), (Plan *) hj,
						cx->rtable, &innerRelid, &innerAttno))
	{
		Relation	innerRel = table_open(innerRelid, NoLock);
		double		innerDistinct = rf_column_ndistinct(innerRel, innerAttno);

		table_close(innerRel, NoLock);
		if (innerDistinct > 0)
			innerKeys = Min(innerKeys, innerDistinct);
	}

	*fraction = Min(1.0, innerKeys / probeDistinct);
	return true;
}

static bool
rf_motion_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Motion))
		return true;
	return plan_tree_walker(node, rf_motion_walker, context, false);
}

/*
 * The rows of a plan node as the segment that runs it sees them. A plan's
 * row estimates are a segment's share, and so are GPORCA's -- except in a
 * slice it runs on one segment, as it does a join of replicated tables,
 * where a replicated table read right there counts once per segment of the
 * cluster: the number of segments times what the one segment reads. The
 * gates below take rows for a segment's, so that estimate is divided back.
 * A subtree with a Motion in it gathers what other segments read and is
 * estimated right.
 */
static double
rf_plan_rows(RuntimeFilterPlanContext *cx, Plan *plan)
{
	double		rows = plan->plan_rows;
	int			nseg = getgpsegmentCount();
	PlanSlice  *slice;

	if (!cx->orcaRows || nseg <= 1 || cx->glob->slices == NULL ||
		cx->sliceIndex < 0 || cx->sliceIndex >= cx->glob->numSlices)
		return rows;
	slice = &cx->glob->slices[cx->sliceIndex];
	if (slice->gangType != GANGTYPE_SINGLETON_READER &&
		!((slice->gangType == GANGTYPE_PRIMARY_READER ||
		   slice->gangType == GANGTYPE_PRIMARY_WRITER) &&
		  slice->numsegments == 1))
		return rows;
	if (rf_motion_walker((Node *) plan, cx))
		return rows;
	return rows / nseg;
}

/*
 * Plan a filter for this join whatever the estimates say? Under
 * whpg_runtime_filter_mode = eager, always; under auto, when the probe
 * column has no statistics to estimate by (a table never analyzed), since
 * the gates would then judge on defaults. The rows that arrive decide the
 * rest (nodeHashjoin.c): the filter's size, whether it is published, and
 * its rest while it proves useless.
 */
static bool
rf_eager(RuntimeFilterPlanContext *cx, SeqScan *scan, Node *scanKey)
{
	RangeTblEntry *rte;
	Relation	rel;
	bool		stats;

	if (whpg_runtime_filter_mode == RF_MODE_EAGER)
		return true;
	if (whpg_runtime_filter_mode != RF_MODE_AUTO)
		return false;

	while (scanKey != NULL && IsA(scanKey, RelabelType))
		scanKey = (Node *) ((RelabelType *) scanKey)->arg;
	if (scanKey == NULL || !IsA(scanKey, Var) ||
		scan->scan.scanrelid <= 0 ||
		scan->scan.scanrelid > list_length(cx->rtable))
		return false;
	rte = rt_fetch(scan->scan.scanrelid, cx->rtable);
	if (rte->rtekind != RTE_RELATION)
		return false;
	rel = table_open(rte->relid, NoLock);	/* the query holds its lock */
	stats = rf_column_ndistinct(rel, ((Var *) scanKey)->varattno) > 0;
	table_close(rel, NoLock);
	return !stats;
}

static void
rf_consider_hashjoin(RuntimeFilterPlanContext *cx, HashJoin *hj)
{
	Plan	   *outer = outerPlan(hj);
	Plan	   *inner = innerPlan(hj);
	int			nkeys;
	int			best = -1;
	int			declined = RF_DECLINE_NONE;
	SeqScan    *scan = NULL;
	Node	   *scanKey = NULL;
	bool		dynamic = false;
	Oid			opno = InvalidOid;
	double		sel = 1.0;
	double		probeRows;
	double		buildRows;
	double		joinSel;
	bool		eager;
	Oid			collation;
	int			paramid;
	Expr	   *clause;
	bool		wasStaged;
	List	   *stagedAttnos;
	int			stagedWidth;
	double		stagedSel;

	if (hj->rf_paramids != NIL)
		return;
	if (!rf_join_type_ok(hj->join.jointype))
	{
		hj->rf_decline = RF_DECLINE_JOIN_TYPE;
		return;
	}
	if (hj->hashqualclauses != NIL)	/* IS NOT DISTINCT FROM: NULLs match */
	{
		hj->rf_decline = RF_DECLINE_NULL_MATCHING;
		return;
	}
	nkeys = list_length(hj->hashclauses);
	if (nkeys == 0 || nkeys != list_length(hj->hashkeys) ||
		nkeys != list_length(hj->hashoperators) ||
		outer == NULL || inner == NULL || !IsA(inner, Hash))
	{
		hj->rf_decline = RF_DECLINE_SHAPE;
		return;
	}
	if (hj->join.plan.parallel_aware || inner->parallel_aware)
	{
		hj->rf_decline = RF_DECLINE_PARALLEL;
		return;
	}

	probeRows = rf_plan_rows(cx, outer);
	buildRows = rf_plan_rows(cx, inner);
	hj->rf_probe_rows = probeRows;
	joinSel = outer->plan_rows > 0 ?
		hj->join.plan.plan_rows / outer->plan_rows : 1.0;
	joinSel = Max(0.0, Min(1.0, joinSel));

	/*
	 * Every key that is a column of the probe scan, under a hashable and
	 * strict operator, could be the filter's. A join on one key takes it,
	 * with the join's survivor estimate. A join on several takes the key
	 * expected to let the fewest probe rows through, each key's share
	 * estimated by itself and never put below the join's. When no key
	 * qualifies, the first one's reason stands for the join's.
	 */
	for (int i = 0; i < nkeys; i++)
	{
		SeqScan    *keyScan;
		Node	   *keyScanKey;
		bool		keyDynamic;
		Oid			keyOp = InvalidOid;
		double		keySel = 1.0;
		int			keyDecline = RF_DECLINE_NONE;

		switch (rf_find_probe_scan(outer, (Node *) list_nth(hj->hashkeys, i),
								   &keyScan, &keyScanKey, &keyDynamic))
		{
			case RF_PROBE_FOUND:
				break;
			case RF_PROBE_MOTION:
				/*
				 * The probe rows are redistributed to this slice. Getting
				 * the filter to the scan that produces them -- as a tuple
				 * through a plan rewrite, or through the interconnect -- is
				 * the cross-segment phase; until then, no filter.
				 */
				keyDecline = RF_DECLINE_PROBE_MOTION;
				break;
			case RF_PROBE_UNSUPPORTED:
				keyDecline = RF_DECLINE_PROBE_NOT_SCAN;
				break;
		}

		if (keyDecline == RF_DECLINE_NONE)
		{
			bool		opStrict;

			/*
			 * The clause carries the hash operator; the probe hashes the key
			 * with its left-hand hash function, as ExecHashTableCreate()
			 * hashes the probe side, since the hash clause's left-hand
			 * operand is the outer key. Whether the key can be so hashed is
			 * the one test the probe and the Staged Scan make too
			 * (runtimefilter_key_hashable()), so the three cannot disagree.
			 *
			 * The filter rejects a NULL probe key, as the join does -- but
			 * the join only does so for a strict hash operator
			 * (ExecHashGetHashValue()); a non-strict one hashes NULL as 0
			 * and may match it. Such a key gets no filter.
			 */
			keyOp = list_nth_oid(hj->hashoperators, i);
			if (!runtimefilter_key_hashable(keyOp, exprType(keyScanKey),
											NULL, &opStrict))
				keyDecline = RF_DECLINE_OP_NOT_HASHABLE;
			else if (!opStrict)
				keyDecline = RF_DECLINE_OP_NOT_STRICT;
		}

		if (keyDecline == RF_DECLINE_NONE)
		{
			if (nkeys == 1)
				keySel = joinSel;
			else if (rf_key_match_fraction(cx, hj, i, keyScan, keyScanKey,
										   probeRows, buildRows, &keySel))
				keySel = Max(keySel, joinSel);
			else if (rf_eager(cx, keyScan, keyScanKey))
				keySel = joinSel;	/* no statistics: the first such key will do */
			else
				keyDecline = RF_DECLINE_KEY_STATS;
		}

		if (keyDecline != RF_DECLINE_NONE)
		{
			if (declined == RF_DECLINE_NONE)
				declined = keyDecline;
			continue;
		}
		if (best < 0 || keySel < sel)
		{
			best = i;
			scan = keyScan;
			scanKey = keyScanKey;
			dynamic = keyDynamic;
			opno = keyOp;
			sel = keySel;
		}
	}
	if (best < 0)
	{
		hj->rf_decline = declined;
		return;
	}
	if (scan->scan.plan.parallel_aware)
	{
		hj->rf_decline = RF_DECLINE_PARALLEL;
		return;
	}

	/*
	 * Worth it? A small probe gains nothing; a probe most of whose rows
	 * match pays the probes for little -- by the estimates, which the mode
	 * says how far to trust (rf_eager()). An inner side too large for a
	 * filter within the size limit would only be dropped at run time, after
	 * a pass over it, in any mode.
	 */
	eager = rf_eager(cx, scan, scanKey);
	if (!eager &&
		(probeRows < (double) whpg_runtime_filter_min_probe_rows || probeRows <= 0))
	{
		hj->rf_decline = RF_DECLINE_PROBE_SMALL;
		return;
	}
	if (!eager && sel > whpg_runtime_filter_max_selectivity)
	{
		hj->rf_decline = (nkeys == 1) ? RF_DECLINE_UNSELECTIVE
			: RF_DECLINE_KEYS_UNSELECTIVE;
		return;
	}
	if (!runtimefilter_fits(buildRows))
	{
		hj->rf_decline = RF_DECLINE_BUILD_LARGE;
		return;
	}
	collation = list_length(hj->hashcollations) > best ?
		list_nth_oid(hj->hashcollations, best) : InvalidOid;

	/* the Param the join will set */
	paramid = list_length(*cx->paramExecTypes);
	*cx->paramExecTypes = lappend_oid(*cx->paramExecTypes, INTERNALOID);

	clause = runtimefilter_make_clause(paramid, (Expr *) scanKey, opno,
									   collation, sel);
	rf_add_clause(scan, clause, sel);

	/*
	 * With a new clause in its predicate, an AOCO scan may now be worth
	 * running as a Staged Scan, or a staged one may want another stage
	 * order; the same cost model decides as for any other clause.
	 *
	 * It may also decline a scan it accepted with the clauses it had. The
	 * Staged Scan model re-costs a single filter column as if it were
	 * clustered, because a single-stage Phase A can fall back to the
	 * ordinary scan when the survivors turn out dense, while a multi-column
	 * predicate is taken at its pessimistic estimate, since it cannot. A
	 * second filter column can thus cost the scan the staged read of its
	 * first key -- Phase A's fast path over that column, the other columns
	 * fetched for the survivors only -- for one more clause in the ordinary
	 * path, and that trade loses every time it was measured (TPC-DS q56,
	 * q88, q90, q96: 6 to 22 points of the gain given back). Such a clause
	 * is left out, and the scan keeps the filters it had. The joins are
	 * visited bottom-up, so the filter nearest the scan -- the lowest join,
	 * which the optimizer placed there for its selectivity -- is the one
	 * kept.
	 */
	wasStaged = scan->staged.enabled;
	stagedAttnos = scan->staged.filterAttnos;
	stagedWidth = scan->staged.phaseAWidth;
	stagedSel = scan->staged.phaseASel;
	stagedscan_decide_plan_scan(cx->glob, cx->rtable, scan, dynamic);
	if (wasStaged && !scan->staged.enabled)
	{
		scan->scan.plan.qual = list_delete_ptr(scan->scan.plan.qual, clause);
		scan->staged.enabled = true;
		scan->staged.filterAttnos = stagedAttnos;
		scan->staged.phaseAWidth = stagedWidth;
		scan->staged.phaseASel = stagedSel;
		*cx->paramExecTypes = list_truncate(*cx->paramExecTypes, paramid);
		hj->rf_decline = RF_DECLINE_STAGED_SCAN;
		return;
	}
	hj->rf_paramids = lappend_int(hj->rf_paramids, paramid);
	hj->rf_keyidx = best;
}

static bool
rf_plan_walker(Node *node, void *context)
{
	RuntimeFilterPlanContext *cx = (RuntimeFilterPlanContext *) context;

	if (node == NULL)
		return false;

	/*
	 * Children first: in a chain of joins over one scan the lowest join's
	 * filter is decided before the ones above it (see rf_consider_hashjoin()).
	 * The plan is in its final form, with one range table for it and its
	 * subplans, so a SubqueryScan's or a TableFunctionScan's plan is entered
	 * like any other node's; SubPlans are walked by
	 * runtimefilter_decide_plan(), once each. Below a Motion the nodes run
	 * in the slice the Motion sends from (rf_plan_rows() asks).
	 */
	if (IsA(node, Motion))
	{
		int			saved = cx->sliceIndex;

		cx->sliceIndex = ((Motion *) node)->motionID;
		(void) plan_tree_walker(node, rf_plan_walker, context, false);
		cx->sliceIndex = saved;
		return false;
	}
	(void) plan_tree_walker(node, rf_plan_walker, context, false);

	if (IsA(node, HashJoin))
		rf_consider_hashjoin(cx, (HashJoin *) node);

	return false;
}

/*
 * Decide runtime filters for a plan in its final form: 'rtable' is the one
 * range table the plan's and the 'subplans'' scanrelids index (GPORCA's
 * plans come so; the Postgres planner's are so after set_plan_references()),
 * and the hash keys refer to the children's target lists (OUTER_VAR,
 * INNER_VAR). 'paramExecTypes' is the list a new Param's type is appended
 * to. The slice table (glob->slices) is built; 'orcaRows' says the row
 * estimates are GPORCA's (rf_plan_rows()).
 */
void
runtimefilter_decide_plan(PlannerGlobal *glob, Plan *plan, List *subplans,
						  List *rtable, List **paramExecTypes,
						  bool hasRowSecurity, bool orcaRows)
{
	RuntimeFilterPlanContext cx;
	PlannerInfo *root;
	ListCell   *lc;
	int			i;

	if (!whpg_enable_runtime_filter)
		return;

	/*
	 * The filter clause cannot fail, but the Staged Scan decision it
	 * triggers cannot see a policy's clauses apart from the user's here;
	 * leave plans under row-level security alone, as that pass does.
	 */
	if (hasRowSecurity)
		return;

	/*
	 * plan_tree_walker() looks a SubPlan's plan up through the walker base
	 * whether or not it recurses into it, so the base must know the
	 * subplans: a PlannerInfo over glob is all it reads.
	 */
	root = makeNode(PlannerInfo);
	root->glob = glob;
	planner_init_plan_tree_base(&cx.base, root);
	cx.glob = glob;
	cx.rtable = rtable;
	cx.paramExecTypes = paramExecTypes;
	cx.orcaRows = orcaRows;

	/*
	 * The plan's top node runs in slice 0; a subplan's where it is called
	 * (subplan_sliceIds, one entry per subplan, -1 where none is known).
	 */
	cx.sliceIndex = 0;
	(void) rf_plan_walker((Node *) plan, &cx);
	i = 0;
	foreach(lc, subplans)
	{
		cx.sliceIndex = (glob->subplan_sliceIds != NULL) ?
			glob->subplan_sliceIds[i] : -1;
		(void) rf_plan_walker((Node *) lfirst(lc), &cx);
		i++;
	}
}
