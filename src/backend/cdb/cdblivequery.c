/*-------------------------------------------------------------------------
 *
 * cdblivequery.c
 *    Live query plan viewer.
 *
 *    Plan registry in shmem, per-segment metrics collector,
 *    and whpg_live_query_plan() viewer function.
 *
 * Architecture (see design doc for full picture):
 *
 *   ExecutorStart (QD only, gp_enable_query_metrics=on):
 *     -> WhpgPlanRegistryAlloc() serialises the PlannedStmt into a
 *        WhpgLiveQuerySlot in shared memory.
 *
 *   ExecutorEnd (QD only):
 *     -> WhpgPlanRegistryFree() marks the slot as free.
 *
 *   whpg_live_query_plan(pid):
 *     1. Find the slot for the target backend.
 *     2. Read QD-local InstrumentationSlot entries.
 *     3. Fan out whpg_collect_segment_metrics(ssid, ccnt) to segments.
 *     4. Stitch plan tree + metrics and emit rows.
 *
 *   whpg_collect_segment_metrics(ssid, ccnt):
 *     Runs on each segment.  Scans the local InstrumentationSlot ring
 *     and returns one row per matching (ssid, ccnt) entry.
 *
 * Copyright (c) 2024-Present, EnterpriseDB Corporation.
 *
 * src/backend/cdb/cdblivequery.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "cdb/cdblivequery.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbvars.h"
#include "executor/execdesc.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"
#include "libpq-fe.h"
#include "port/atomics.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

PG_FUNCTION_INFO_V1(whpg_live_query_plan);
PG_FUNCTION_INFO_V1(whpg_collect_segment_metrics);

/* ----------------------------------------------------------------
 * GUC variables
 * ----------------------------------------------------------------
 */
int			whpg_max_live_query_slots = 0;	/* 0 = use MaxBackends at startup */
int			whpg_avg_plan_bytes = 131072;	/* per-slot arena size */

/* Global pointer to the plan registry shared memory */
WhpgPlanRegistry *WhpgRegistry = NULL;

/* ----------------------------------------------------------------
 * Helpers to access the slot array that follows the header
 * ----------------------------------------------------------------
 */
#define WHPG_SLOT_ARRAY(reg) \
	((WhpgLiveQuerySlot *)((char *)(reg) + sizeof(WhpgPlanRegistry)))

#define WHPG_SLOT(reg, i) \
	(&WHPG_SLOT_ARRAY(reg)[i])

/* ----------------------------------------------------------------
 * Node type -> human-readable string
 *
 * EXPLAIN uses a PlanState walker and a big switch on PlanState types.
 * We operate on the static Plan, so we switch on the Plan NodeTag.
 * Unknown tags fall through to a numeric fallback.
 * ----------------------------------------------------------------
 */
const char *
WhpgNodeTypeStr(NodeTag tag)
{
	switch (tag)
	{
		case T_SeqScan:				return "Seq Scan";
		case T_SampleScan:			return "Sample Scan";
		case T_IndexScan:			return "Index Scan";
		case T_IndexOnlyScan:		return "Index Only Scan";
		case T_BitmapIndexScan:		return "Bitmap Index Scan";
		case T_BitmapHeapScan:		return "Bitmap Heap Scan";
		case T_TidScan:				return "Tid Scan";
		case T_SubqueryScan:		return "Subquery Scan";
		case T_FunctionScan:		return "Function Scan";
		case T_TableFuncScan:		return "Table Function Scan";
		case T_ValuesScan:			return "Values Scan";
		case T_CteScan:				return "CTE Scan";
		case T_NamedTuplestoreScan:	return "Named Tuplestore Scan";
		case T_WorkTableScan:		return "WorkTable Scan";
		case T_ForeignScan:			return "Foreign Scan";
		case T_CustomScan:			return "Custom Scan";
		case T_NestLoop:			return "Nested Loop";
		case T_MergeJoin:			return "Merge Join";
		case T_HashJoin:			return "Hash Join";
		case T_Gather:				return "Gather";
		case T_GatherMerge:			return "Gather Merge";
		case T_Hash:				return "Hash";
		case T_Material:			return "Materialize";
		case T_Sort:				return "Sort";
		case T_Agg:					return "Aggregate";
		case T_WindowAgg:			return "WindowAgg";
		case T_Unique:				return "Unique";
		case T_SetOp:				return "SetOp";
		case T_LockRows:			return "Lock Rows";
		case T_Limit:				return "Limit";
		case T_Append:				return "Append";
		case T_MergeAppend:			return "Merge Append";
		case T_RecursiveUnion:		return "Recursive Union";
		case T_BitmapAnd:			return "BitmapAnd";
		case T_BitmapOr:			return "BitmapOr";
		case T_Result:				return "Result";
		case T_ProjectSet:			return "ProjectSet";
		case T_ModifyTable:			return "Modify Table";
		case T_Motion:				return "Motion";
		case T_Sequence:			return "Sequence";
		case T_ShareInputScan:		return "Share Input Scan";
		default:
		{
			/* Static buffer: caller uses the string immediately before next call */
			static char buf[32];
			snprintf(buf, sizeof(buf), "NodeTag(%u)", (unsigned) tag);
			return buf;
		}
	}
}

/* ----------------------------------------------------------------
 * Shmem sizing and initialisation
 * ----------------------------------------------------------------
 */

Size
WhpgPlanRegistryShmemSize(void)
{
	int		nslots;

	if (!gp_enable_query_metrics)
		return 0;

	nslots = (whpg_max_live_query_slots > 0)
		? whpg_max_live_query_slots
		: MaxBackends;

	return add_size(sizeof(WhpgPlanRegistry),
					mul_size((Size) nslots, sizeof(WhpgLiveQuerySlot)));
}

void
WhpgPlanRegistryShmemInit(void)
{
	Size		size;
	bool		found;
	int			nslots;

	if (!gp_enable_query_metrics)
		return;

	size = WhpgPlanRegistryShmemSize();
	if (size == 0)
		return;

	WhpgRegistry = (WhpgPlanRegistry *)
		ShmemInitStruct("WhpgPlanRegistry", size, &found);

	if (!found)
	{
		nslots = (whpg_max_live_query_slots > 0)
			? whpg_max_live_query_slots
			: MaxBackends;

		memset(WhpgRegistry, 0, size);
		WhpgRegistry->n_slots = nslots;
	}
}

/* ----------------------------------------------------------------
 * Plan tree walker
 *
 * Recursively serialises Plan nodes into the slot's nodes[] array.
 * We walk the static Plan tree (from PlannedStmt) so this can be
 * called before InitPlan populates the PlanState.
 * ----------------------------------------------------------------
 */

static void walk_plan_node(Plan *plan, int parent_id, int slice_id,
						   WhpgLiveQuerySlot *slot);

static void
walk_plan_list(List *plans, int parent_id, int slice_id,
			   WhpgLiveQuerySlot *slot)
{
	ListCell   *lc;

	foreach(lc, plans)
		walk_plan_node((Plan *) lfirst(lc), parent_id, slice_id, slot);
}

static void
walk_plan_node(Plan *plan, int parent_id, int slice_id,
			   WhpgLiveQuerySlot *slot)
{
	WhpgPlanNodeSummary *summary;
	int			child_slice_id;
	int			my_id;

	if (plan == NULL)
		return;

	if (slot->n_nodes >= WHPG_MAX_PLAN_NODES)
	{
		slot->plan_truncated = true;
		return;
	}

	summary = &slot->nodes[slot->n_nodes++];
	summary->plan_node_id = plan->plan_node_id;
	summary->parent_plan_node_id = parent_id;
	summary->slice_id = slice_id;
	summary->node_type = (uint16) nodeTag(plan);
	summary->motion_kind = 0;
	summary->plan_rows = plan->plan_rows;

	my_id = plan->plan_node_id;

	if (IsA(plan, Motion))
		summary->motion_kind = (uint8) ((Motion *) plan)->motionType;

	/* Standard binary children */
	walk_plan_node(plan->lefttree, my_id, slice_id, slot);
	walk_plan_node(plan->righttree, my_id, slice_id, slot);

	/* Multi-child plan types */
	switch (nodeTag(plan))
	{
		case T_Append:
			walk_plan_list(((Append *) plan)->appendplans,
						   my_id, slice_id, slot);
			break;
		case T_MergeAppend:
			walk_plan_list(((MergeAppend *) plan)->mergeplans,
						   my_id, slice_id, slot);
			break;
		case T_BitmapAnd:
			walk_plan_list(((BitmapAnd *) plan)->bitmapplans,
						   my_id, slice_id, slot);
			break;
		case T_BitmapOr:
			walk_plan_list(((BitmapOr *) plan)->bitmapplans,
						   my_id, slice_id, slot);
			break;
		case T_SubqueryScan:
			walk_plan_node(((SubqueryScan *) plan)->subplan,
						   my_id, slice_id, slot);
			break;
		case T_Sequence:
			walk_plan_list(((Sequence *) plan)->subplans,
						   my_id, slice_id, slot);
			break;
		default:
			break;
	}
}

/* ----------------------------------------------------------------
 * WhpgPlanRegistryAlloc
 *
 * Called at the end of standard_ExecutorStart on the QD.
 * Finds a free slot and serialises the plan tree into it.
 * ----------------------------------------------------------------
 */
void
WhpgPlanRegistryAlloc(QueryDesc *queryDesc)
{
	WhpgLiveQuerySlot *slot;
	int			i;

	/* Only on QD, only when metrics are enabled */
	if (Gp_role != GP_ROLE_DISPATCH)
		return;
	if (!gp_enable_query_metrics || WhpgRegistry == NULL)
		return;

	/* Find a free slot (linear scan) */
	slot = NULL;
	for (i = 0; i < WhpgRegistry->n_slots; i++)
	{
		if (!WHPG_SLOT(WhpgRegistry, i)->in_use)
		{
			slot = WHPG_SLOT(WhpgRegistry, i);
			break;
		}
	}

	if (slot == NULL)
	{
		ereport(LOG,
				(errmsg("whpg_live_query_plan: plan registry full "
						"(whpg_max_live_query_slots=%d); "
						"query will not be visible in live view",
						WhpgRegistry->n_slots)));
		return;
	}

	/* Populate the slot */
	memset(slot, 0, sizeof(*slot));
	slot->session_id = gp_session_id;
	slot->command_count = gp_command_count;
	slot->backend_pid = MyProcPid;
	slot->queryid = (queryDesc->plannedstmt != NULL)
					? queryDesc->plannedstmt->queryId : 0;
	slot->dbid = MyDatabaseId;
	slot->userid = GetUserId();
	slot->start_time = GetCurrentTimestamp();
	slot->n_nodes = 0;
	slot->plan_truncated = false;

	/* Walk the plan tree */
	if (queryDesc->plannedstmt && queryDesc->plannedstmt->planTree)
		walk_plan_node(queryDesc->plannedstmt->planTree,
					   -1 /* root has no parent */,
					   0  /* root is slice 0 */,
					   slot);

	if (slot->plan_truncated)
		ereport(LOG,
				(errmsg("whpg_live_query_plan: plan has >%d nodes; "
						"truncated in live view (query still runs)",
						WHPG_MAX_PLAN_NODES)));

	/*
	 * Mark in_use last with a write barrier so a concurrent reader on
	 * another CPU sees a fully written slot before it sees in_use=true.
	 */
	pg_write_barrier();
	slot->in_use = true;
}

/* ----------------------------------------------------------------
 * WhpgPlanRegistryFree
 *
 * Called at the beginning of standard_ExecutorEnd on the QD.
 * ----------------------------------------------------------------
 */
void
WhpgPlanRegistryFree(QueryDesc *queryDesc)
{
	WhpgLiveQuerySlot *slot;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;
	if (!gp_enable_query_metrics || WhpgRegistry == NULL)
		return;

	slot = WhpgFindActiveSlot();
	if (slot == NULL)
		return;

	slot->in_use = false;
	pg_write_barrier();
}

/* ----------------------------------------------------------------
 * WhpgFindActiveSlot
 *
 * Returns the plan registry slot for the currently-executing query
 * on this backend, or NULL if none is registered.  Used by both
 * WhpgPlanRegistryFree (cdblivequery.c) and WhpgEmitQueryHistory
 * (cdbhistory.c) to avoid duplicating the linear scan.
 * ----------------------------------------------------------------
 */
WhpgLiveQuerySlot *
WhpgFindActiveSlot(void)
{
	int		i;

	if (WhpgRegistry == NULL)
		return NULL;

	for (i = 0; i < WhpgRegistry->n_slots; i++)
	{
		WhpgLiveQuerySlot *slot = WHPG_SLOT(WhpgRegistry, i);

		if (slot->in_use &&
			slot->session_id == gp_session_id &&
			slot->command_count == gp_command_count)
			return slot;
	}
	return NULL;
}

/* ----------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------
 */

static WhpgLiveQuerySlot *
find_slot_by_pid(int pid)
{
	int			i;

	if (WhpgRegistry == NULL)
		return NULL;

	for (i = 0; i < WhpgRegistry->n_slots; i++)
	{
		WhpgLiveQuerySlot *slot = WHPG_SLOT(WhpgRegistry, i);

		if (slot->in_use && slot->backend_pid == pid)
			return slot;
	}
	return NULL;
}

static WhpgPlanNodeSummary *
find_plan_node(WhpgLiveQuerySlot *slot, int plan_node_id)
{
	int			i;

	for (i = 0; i < slot->n_nodes; i++)
	{
		if (slot->nodes[i].plan_node_id == plan_node_id)
			return &slot->nodes[i];
	}
	return NULL;
}

/* ----------------------------------------------------------------
 * whpg_collect_segment_metrics(ssid int4, ccnt int4)
 *
 * Set-returning function that runs on each segment.  Scans the local
 * InstrumentationSlot ring and returns one row per slot matching
 * (session_id == ssid AND command_count == ccnt).
 *
 * Output columns:
 *   plan_node_id int4, segindex int4, running bool,
 *   tuples int8, actual_ms float8,
 *   execmem_bytes int8, workmem_bytes int8,
 *   spill_bytes int8, bufusage_blocks int8
 * ----------------------------------------------------------------
 */

#define WHPG_COLLECT_SEG_NATTS	9

#define GET_INSTR_SLOT(index) \
	((InstrumentationSlot *)((InstrumentGlobal) + 1) + (index))

Datum
whpg_collect_segment_metrics(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int32		ssid;
	int32		ccnt;
	int32	   *scan_idx;

	ssid = PG_GETARG_INT32(0);
	ccnt = PG_GETARG_INT32(1);

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldctx;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(WHPG_COLLECT_SEG_NATTS);
		TupleDescInitEntry(tupdesc, 1, "plan_node_id",	 INT4OID,   -1, 0);
		TupleDescInitEntry(tupdesc, 2, "segindex",		 INT4OID,   -1, 0);
		TupleDescInitEntry(tupdesc, 3, "running",		 BOOLOID,   -1, 0);
		TupleDescInitEntry(tupdesc, 4, "tuples",		 INT8OID,   -1, 0);
		TupleDescInitEntry(tupdesc, 5, "actual_ms",		 FLOAT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 6, "execmem_bytes",	 INT8OID,   -1, 0);
		TupleDescInitEntry(tupdesc, 7, "workmem_bytes",	 INT8OID,   -1, 0);
		TupleDescInitEntry(tupdesc, 8, "spill_bytes",	 INT8OID,   -1, 0);
		TupleDescInitEntry(tupdesc, 9, "bufusage_blocks",INT8OID,   -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		scan_idx = (int32 *) palloc0(sizeof(int32));
		funcctx->user_fctx = scan_idx;

		MemoryContextSwitchTo(oldctx);
	}

	funcctx = SRF_PERCALL_SETUP();
	scan_idx = (int32 *) funcctx->user_fctx;

	if (InstrumentGlobal == NULL)
		SRF_RETURN_DONE(funcctx);

	while (*scan_idx < (int32) InstrShmemNumSlots())
	{
		InstrumentationSlot *isl = GET_INSTR_SLOT(*scan_idx);
		(*scan_idx)++;

		if (SlotIsEmpty(isl))
			continue;
		if (isl->ssid != ssid || isl->ccnt != (int32) ccnt)
			continue;

		{
			Datum		values[WHPG_COLLECT_SEG_NATTS];
			bool		nulls[WHPG_COLLECT_SEG_NATTS];
			Instrumentation *d = &isl->data;
			int64		tuples;
			int64		spill_bytes;
			int64		bufblocks;
			HeapTuple	htup;

			memset(nulls, 0, sizeof(nulls));

			/* tuples = in-progress cycle + completed cycles */
			tuples = whpg_i64_add_sat(
						whpg_u64_to_i64_sat(d->tuplecount),
						whpg_u64_to_i64_sat(d->ntuples));

			/* sortSpaceUsed is KiB; convert to bytes */
			spill_bytes = (int64) d->sortSpaceUsed * 1024LL;

			/* shared blocks hit + read is our "blocks touched" proxy */
			bufblocks = whpg_i64_add_sat(
						(int64) d->bufusage.shared_blks_hit,
						(int64) d->bufusage.shared_blks_read);

			values[0] = Int32GetDatum((int32) isl->nid);
			values[1] = Int32GetDatum((int32) isl->segid);
			values[2] = BoolGetDatum(d->running);
			values[3] = Int64GetDatum(tuples);
			values[4] = Float8GetDatum(d->total * 1000.0);	/* sec -> ms */
			values[5] = Int64GetDatum(whpg_dbl_to_i64_sat(d->execmemused));
			values[6] = Int64GetDatum(whpg_dbl_to_i64_sat(d->workmemused));
			values[7] = Int64GetDatum(spill_bytes);
			values[8] = Int64GetDatum(bufblocks);

			htup = heap_form_tuple(funcctx->tuple_desc, values, nulls);
			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(htup));
		}
	}

	SRF_RETURN_DONE(funcctx);
}

/* ----------------------------------------------------------------
 * whpg_live_query_plan(pid int4)
 *
 * Viewer function: returns the live execution plan of the query
 * running on the QD backend identified by `pid`, augmented with
 * per-node, per-segment counters collected from all segments.
 *
 * Permissions: superuser, or the role that owns the target backend
 * (same model as pg_log_backend_memory_contexts).
 * ----------------------------------------------------------------
 */

#define WHPG_LIVE_NATTS		14

/*
 * In-memory accumulator for one (plan_node_id, segindex) pair.
 */
typedef struct LiveRow
{
	int32		slice_id;
	int32		plan_node_id;
	int32		parent_node_id;
	uint16		node_type;
	int32		segindex;
	double		plan_rows;
	bool		running;
	int64		tuples;			/* per-segment */
	int64		ntuples_total;	/* sum across all segments, filled later */
	double		actual_ms;
	int64		execmem_bytes;
	int64		workmem_bytes;
	int64		spill_bytes;
	int64		bufusage_blocks;
} LiveRow;

/*
 * Read QD-local InstrumentationSlot entries for (ssid, ccnt) and
 * append LiveRow entries to rows[].  Returns number of rows added.
 */
static int
collect_local_metrics(int ssid, int ccnt, int segindex,
					  LiveRow *rows, int max_rows,
					  WhpgLiveQuerySlot *slot)
{
	int			nrows = 0;
	int			i;

	if (InstrumentGlobal == NULL)
		return 0;

	for (i = 0; i < (int) InstrShmemNumSlots() && nrows < max_rows; i++)
	{
		InstrumentationSlot *isl = GET_INSTR_SLOT(i);
		WhpgPlanNodeSummary *pn;
		Instrumentation *d;
		LiveRow    *r;

		if (SlotIsEmpty(isl))
			continue;
		if (isl->ssid != ssid || isl->ccnt != (int32) ccnt)
			continue;

		pn = find_plan_node(slot, (int) isl->nid);
		if (pn == NULL)
			continue;

		d = &isl->data;
		r = &rows[nrows++];

		r->slice_id = pn->slice_id;
		r->plan_node_id = pn->plan_node_id;
		r->parent_node_id = pn->parent_plan_node_id;
		r->node_type = pn->node_type;
		r->plan_rows = pn->plan_rows;
		r->segindex = segindex;
		r->running = d->running;
		r->tuples = whpg_i64_add_sat(
					whpg_u64_to_i64_sat(d->tuplecount),
					whpg_u64_to_i64_sat(d->ntuples));
		r->ntuples_total = 0;
		r->actual_ms = d->total * 1000.0;
		r->execmem_bytes = whpg_dbl_to_i64_sat(d->execmemused);
		r->workmem_bytes = whpg_dbl_to_i64_sat(d->workmemused);
		r->spill_bytes = (int64) d->sortSpaceUsed * 1024LL;
		r->bufusage_blocks = whpg_i64_add_sat(
					(int64) d->bufusage.shared_blks_hit,
					(int64) d->bufusage.shared_blks_read);
	}

	return nrows;
}

Datum
whpg_live_query_plan(PG_FUNCTION_ARGS)
{
	int32		target_pid = PG_GETARG_INT32(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	WhpgLiveQuerySlot *slot;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext old_ctx;
	char		sql[256];
	CdbPgResults cdb_results = {NULL, 0};
	LiveRow    *rows;
	int			nrows = 0;
	int			max_rows;
	int			i;

	/* ---- permission check ----------------------------------------
	 * Allow superuser, or the role that owns the target backend.
	 * We use BackendPidGetProc() to look up the backend's roleId,
	 * which is the same approach used by pg_log_backend_memory_contexts.
	 * ----------------------------------------------------------------
	 */
	if (!superuser())
	{
		PGPROC	   *proc = BackendPidGetProc(target_pid);

		if (proc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("PID %d is not a PostgreSQL backend process",
							target_pid)));

		if (proc->roleId != GetUserId())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser or own the target backend process")));
	}

	/* ---- validate return set infrastructure ---- */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	rsinfo->returnMode = SFRM_Materialize;

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	old_ctx = MemoryContextSwitchTo(per_query_ctx);

	tupdesc = CreateTemplateTupleDesc(WHPG_LIVE_NATTS);
	TupleDescInitEntry(tupdesc,  1, "slice_id",		   INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc,  2, "plan_node_id",	   INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc,  3, "parent_node_id",  INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc,  4, "node_type",	   TEXTOID,   -1, 0);
	TupleDescInitEntry(tupdesc,  5, "segindex",		   INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc,  6, "plan_rows",	   FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc,  7, "running",		   BOOLOID,   -1, 0);
	TupleDescInitEntry(tupdesc,  8, "tuples",		   INT8OID,   -1, 0);
	TupleDescInitEntry(tupdesc,  9, "ntuples_total",   INT8OID,   -1, 0);
	TupleDescInitEntry(tupdesc, 10, "actual_ms",	   FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, 11, "execmem_bytes",   INT8OID,   -1, 0);
	TupleDescInitEntry(tupdesc, 12, "workmem_bytes",   INT8OID,   -1, 0);
	TupleDescInitEntry(tupdesc, 13, "spill_bytes",	   INT8OID,   -1, 0);
	TupleDescInitEntry(tupdesc, 14, "bufusage_blocks", INT8OID,   -1, 0);

	BlessTupleDesc(tupdesc);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	/* ---- 1. Find the plan registry slot for this pid ---- */
	if (!gp_enable_query_metrics || WhpgRegistry == NULL)
		goto done;

	slot = find_slot_by_pid(target_pid);
	if (slot == NULL)
		goto done;		/* no running query for this pid */

	/* Over-provision: at most n_nodes rows per segment + coordinator */
	max_rows = slot->n_nodes * (MaxBackends + 1);
	if (max_rows <= 0)
		goto done;

	rows = (LiveRow *) palloc0(mul_size((Size) max_rows, sizeof(LiveRow)));

	/* ---- 2. QD-local InstrumentationSlot entries (coordinator = segindex -1) ---- */
	nrows = collect_local_metrics(slot->session_id, slot->command_count,
								  -1 /* coordinator */,
								  rows, max_rows, slot);

	/* ---- 3. Fan out to all segments ---- */
	snprintf(sql, sizeof(sql),
			 "SELECT plan_node_id,segindex,running,tuples,actual_ms,"
			 "execmem_bytes,workmem_bytes,spill_bytes,bufusage_blocks"
			 " FROM pg_catalog.whpg_collect_segment_metrics(%d,%d)",
			 slot->session_id, slot->command_count);

	PG_TRY();
	{
		CdbDispatchCommand(sql, DF_WITH_SNAPSHOT | DF_CANCEL_ON_ERROR,
						   &cdb_results);
	}
	PG_CATCH();
	{
		cdbdisp_clearCdbPgResults(&cdb_results);
		MemoryContextSwitchTo(old_ctx);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Parse per-segment results */
	for (i = 0; i < cdb_results.numResults; i++)
	{
		struct pg_result *pgres = cdb_results.pg_results[i];
		int			ntup;
		int			t;

		if (PQresultStatus(pgres) != PGRES_TUPLES_OK)
			continue;

		ntup = PQntuples(pgres);
		for (t = 0; t < ntup && nrows < max_rows; t++)
		{
			int32				plan_node_id;
			WhpgPlanNodeSummary *pn;
			LiveRow			   *r;

			plan_node_id = atoi(PQgetvalue(pgres, t, 0));

			pn = find_plan_node(slot, plan_node_id);
			if (pn == NULL)
				continue;

			r = &rows[nrows++];
			r->slice_id = pn->slice_id;
			r->plan_node_id = plan_node_id;
			r->parent_node_id = pn->parent_plan_node_id;
			r->node_type = pn->node_type;
			r->plan_rows = pn->plan_rows;
			r->segindex = atoi(PQgetvalue(pgres, t, 1));
			r->running = (PQgetvalue(pgres, t, 2)[0] == 't');
			r->tuples = atoll(PQgetvalue(pgres, t, 3));
			r->ntuples_total = 0;
			r->actual_ms = atof(PQgetvalue(pgres, t, 4));
			r->execmem_bytes = atoll(PQgetvalue(pgres, t, 5));
			r->workmem_bytes = atoll(PQgetvalue(pgres, t, 6));
			r->spill_bytes = atoll(PQgetvalue(pgres, t, 7));
			r->bufusage_blocks = atoll(PQgetvalue(pgres, t, 8));
		}
	}

	cdbdisp_clearCdbPgResults(&cdb_results);

	/* ---- 4. Compute ntuples_total per plan_node_id ---- */
	for (i = 0; i < nrows; i++)
	{
		int64	total;
		int		j;

		if (rows[i].ntuples_total != 0)
			continue;	/* already stamped by a previous iteration */

		total = 0;
		for (j = 0; j < nrows; j++)
		{
			if (rows[j].plan_node_id == rows[i].plan_node_id)
				total = whpg_i64_add_sat(total, rows[j].tuples);
		}
		for (j = 0; j < nrows; j++)
		{
			if (rows[j].plan_node_id == rows[i].plan_node_id)
				rows[j].ntuples_total = total;
		}
	}

	/* ---- 5. Emit rows ---- */
	for (i = 0; i < nrows; i++)
	{
		LiveRow    *r = &rows[i];
		Datum		values[WHPG_LIVE_NATTS];
		bool		nulls[WHPG_LIVE_NATTS];

		memset(nulls, 0, sizeof(nulls));

		values[0]  = Int32GetDatum(r->slice_id);
		values[1]  = Int32GetDatum(r->plan_node_id);
		values[2]  = Int32GetDatum(r->parent_node_id);
		values[3]  = CStringGetTextDatum(WhpgNodeTypeStr((NodeTag) r->node_type));
		values[4]  = Int32GetDatum(r->segindex);
		values[5]  = Float8GetDatum(r->plan_rows);
		values[6]  = BoolGetDatum(r->running);
		values[7]  = Int64GetDatum(r->tuples);
		values[8]  = Int64GetDatum(r->ntuples_total);
		values[9]  = Float8GetDatum(r->actual_ms);
		values[10] = Int64GetDatum(r->execmem_bytes);
		values[11] = Int64GetDatum(r->workmem_bytes);
		values[12] = Int64GetDatum(r->spill_bytes);
		values[13] = Int64GetDatum(r->bufusage_blocks);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

done:
	tuplestore_donestoring(tupstore);
	MemoryContextSwitchTo(old_ctx);
	PG_RETURN_NULL();
}
