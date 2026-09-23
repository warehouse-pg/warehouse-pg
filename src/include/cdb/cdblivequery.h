/*-------------------------------------------------------------------------
 *
 * cdblivequery.h
 *    Live query plan viewer: shmem plan registry, per-segment metrics
 *    collector, and whpg_live_query_plan() viewer function.
 *
 *
 * Copyright (c) 2024-Present, EnterpriseDB Corporation.
 *
 * src/include/cdb/cdblivequery.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBLIVEQUERY_H
#define CDBLIVEQUERY_H

#include "postgres.h"
#include "fmgr.h"
#include "executor/execdesc.h"
#include "utils/timestamp.h"
#include <math.h>

/* ----------------------------------------------------------------
 * Limits
 * ----------------------------------------------------------------
 */

/*
 * Maximum plan nodes stored per running query in the registry.
 * Plans we have seen top out in the low thousands; 1024 covers the
 * vast majority.  If a query has more nodes the slot is still recorded
 * but plan_truncated is set and nodes beyond 1024 are invisible.
 */
#define WHPG_MAX_PLAN_NODES		1024

/* ----------------------------------------------------------------
 * Per-node compact summary (stored in the plan registry shmem slot)
 * ----------------------------------------------------------------
 */
typedef struct WhpgPlanNodeSummary
{
	int32		plan_node_id;		/* Plan.plan_node_id, unique per query */
	int32		parent_plan_node_id;/* -1 for root */
	int32		slice_id;			/* execution slice this node belongs to */
	uint16		node_type;			/* NodeTag cast to uint16 */
	uint8		motion_kind;		/* MotionType if node is a Motion, else 0 */
	bool		pad;				/* alignment */
	double		plan_rows;			/* optimizer row estimate (our addition) */
} WhpgPlanNodeSummary;				/* 24 bytes */

/* ----------------------------------------------------------------
 * Plan registry slot: one per running query on the QD
 * ----------------------------------------------------------------
 */
typedef struct WhpgLiveQuerySlot
{
	/* Lookup / validity fields */
	int32		session_id;
	int32		command_count;
	int32		backend_pid;
	bool		in_use;
	bool		plan_truncated;		/* plan had > WHPG_MAX_PLAN_NODES nodes */
	char		pad[2];

	/* Query metadata */
	Oid			dbid;
	Oid			userid;
	TimestampTz	start_time;

	/* Number of nodes actually stored */
	int32		n_nodes;

	/* Compact plan tree (fixed-size array) */
	WhpgPlanNodeSummary nodes[WHPG_MAX_PLAN_NODES];
} WhpgLiveQuerySlot;

/* ----------------------------------------------------------------
 * Shared memory header for the plan registry
 * ----------------------------------------------------------------
 */
typedef struct WhpgPlanRegistry
{
	int32		n_slots;	/* total slots allocated (set at startup) */
	int32		pad;
	/* WhpgLiveQuerySlot slots[n_slots] follow immediately */
} WhpgPlanRegistry;

extern WhpgPlanRegistry *WhpgRegistry;

/* ----------------------------------------------------------------
 * GUC variables (declared here, defined in cdblivequery.c)
 * ----------------------------------------------------------------
 */
extern int	whpg_max_live_query_slots;

/* ----------------------------------------------------------------
 * Shmem lifecycle API
 * ----------------------------------------------------------------
 */
extern Size WhpgPlanRegistryShmemSize(void);
extern void WhpgPlanRegistryShmemInit(void);

/* ----------------------------------------------------------------
 * Executor hook API (called from execMain.c)
 * ----------------------------------------------------------------
 */
extern void WhpgPlanRegistryAlloc(QueryDesc *queryDesc);
extern void WhpgPlanRegistryFree(QueryDesc *queryDesc);

/* ----------------------------------------------------------------
 * SQL-callable functions
 * ----------------------------------------------------------------
 */
extern Datum whpg_live_query_plan(PG_FUNCTION_ARGS);
extern Datum whpg_collect_segment_metrics(PG_FUNCTION_ARGS);

/* ----------------------------------------------------------------
 * Saturating arithmetic helpers
 * Defined inline so callers in cdblivequery.c and future files share
 * one implementation without a link dependency.
 * ----------------------------------------------------------------
 */
static inline int64
whpg_u64_to_i64_sat(uint64 v)
{
	return (v > (uint64) INT64_MAX) ? INT64_MAX : (int64) v;
}

static inline int64
whpg_dbl_to_i64_sat(double d)
{
	if (isnan(d) || d <= 0.0)
		return 0;
	if (d >= (double) INT64_MAX)
		return INT64_MAX;
	return (int64) d;
}

static inline int64
whpg_i64_add_sat(int64 a, int64 b)
{
	int64		r;

	if (__builtin_add_overflow(a, b, &r))
		return (b > 0) ? INT64_MAX : INT64_MIN;
	return r;
}

#endif							/* CDBLIVEQUERY_H */
