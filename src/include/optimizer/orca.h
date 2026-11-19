/*-------------------------------------------------------------------------
 *
 * orca.h
 *	  prototypes for the ORCA query planner
 *
 *
 * Portions Copyright (c) 2010-Present, VMware, Inc. or its affiliates
 * Portions Copyright (c) 2005-2009, Greenplum inc
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/include/optimizer/orca.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ORCA_H
#define ORCA_H

#include "pg_config.h"
#include "access/htup.h"
#include "storage/block.h"

#ifdef USE_ORCA

extern PlannedStmt * optimize_query(Query *parse, int cursorOptions, ParamListInfo boundParams);
extern Node *transformGroupedWindows(Node *node, void *context);

// plan_hint_hook generates HintState by parsing a Query.
typedef void *(*plan_hint_hook_type) (Query *parse);
extern PGDLLIMPORT plan_hint_hook_type plan_hint_hook;

struct CustomScanMethods;

/*
 * Version of the ORCA extension hooks below and of the structures they
 * exchange. It changes whenever they do. These hooks are not part of every
 * build of this major version, so an extension should look up
 * orca_hooks_abi_version at run time (dlsym) and set the hooks only if it
 * exists and equals the version the extension was built for.
 */
#define ORCA_HOOKS_ABI_VERSION 1
extern PGDLLIMPORT const int orca_hooks_abi_version;

/*
 * orca_plan_regions_hook lets an extension execute parts of the plan ORCA
 * chose. It is called once per plan, before the plan's DXL is translated to
 * a PlannedStmt, with the DXL and the names of its physical operators in
 * pre-order (the order of their elements in the DXL text): an operator is
 * identified by its number in that order.
 *
 * Each region it returns replaces the subtree at its root operator with a
 * CustomScan of the given methods: custom_private holds the region's dxl, an
 * extension-defined payload, and custom_plans the PG plans translated from
 * its leaves, in the order given. The leaves lie in the root's subtree, and
 * none in another's. A leaf may be the root itself: the region then only
 * converts that operator's output. The CustomScan yields the region's output
 * columns, which the plan above refers to by their DXL column ids.
 *
 * The operators of the root's subtree outside the leaves' subtrees, the
 * region's interior, aren't translated: the extension executes them from
 * their DXL. They may only be results, projections, values scans, constant
 * tables, joins, limits, aggregates, sorts, appends, materializes, windows
 * and asserts, and none may be the root of a subplan (under a scalar
 * expression of its parent). An operator that needs more than its DXL, such
 * as a motion (its slice), a table scan (its range table entry and lock), a
 * CTE producer or consumer, a sequence or a partition selector, must be a
 * leaf or outside the region.
 *
 * The executor treats the CustomScan as any other, without what
 * WarehousePG's executor does for the operators it replaces. The leaves'
 * plan states must be in custom_ps, and the extension must also:
 *
 * - Read a join's inner input to its end before its outer one, when both
 *   have a motion in them, as the executor's joins do (prefetch_inner):
 *   the senders of a motion not being read can hold up those of the other,
 *   on other segments, in a deadlock.
 * - Not rescan a leaf with a motion in it after reading from it, which
 *   raises an error (ExecReScanMotion): a region that is rescanned, such as
 *   one replacing a Materialize, keeps the leaf's rows as a Materialize does
 *   (cdb_shield_child_from_rescans).
 * - Only set up in BeginCustomScan, and start reading in ExecCustomScan: the
 *   coordinator initializes the whole plan, the regions of the segments'
 *   slices too, and runs only its own slice.
 * - Size the memory it sorts, hashes or keeps rows in itself: the resource
 *   manager doesn't count a CustomScan as memory intensive, so
 *   plan.operatorMemKB is small.
 *
 * When the plan above stops reading a region early, the executor squelches
 * its CustomScan (ExecSquelchNode): it calls ShutdownCustomScan, and then
 * squelches custom_ps.
 *
 * A region marked ipc instead yields (segment int4, payload bytea) rows for
 * the motion right above its root, which only routes them: by the segment
 * column, or as a gather or broadcast, and neither sorts nor filters them.
 * Below a redistribute motion, the segment must be the one ORCA's hash spec
 * of that motion puts the row on, since the operators above rely on it: the
 * cdbhash (makeCdbHash, cdbhashinit, cdbhash, cdbhashreduce) of the values
 * of the motion's HashExprList, in order, with the hash function of each
 * HashExpr's Opfamily for its expression's type (cdb_hashproc_in_opfamily),
 * into as many segments as the motion's OutputSegments. A wrong segment in
 * that range silently gives wrong results. Below a random motion it may be
 * any segment in that range; a gather or broadcast ignores it. The executor
 * raises an error for a NULL segment or one outside the range.
 * Such a region has no columns, and its rows must only reach other regions:
 * the motion above it must be a leaf of another region. Below a CTE
 * producer, every consumer of the CTE must be such a leaf, and reads the
 * rows; as the last child of a Sequence, the Sequence passes them on, to
 * such a motion or as a leaf of a region; or it may be a leaf of another
 * region itself. Each of those operators yields the (segment, payload) rows.
 *
 * It isn't called for a query that row-level security applies to: the
 * security quals aren't part of the DXL.
 *
 * Returns NULL, or no regions, to translate the plan as is. The result may
 * be palloc'd in the current memory context. ORCA raises an error, and the
 * query falls back to the PG planner, if the result is malformed.
 */

/* the columns of the (segment, payload) rows of an ipc region */
#define ORCA_REGION_SEGID_COL "region_segid"
#define ORCA_REGION_PAYLOAD_COL "region_payload"

typedef struct OrcaPlanRegionCol
{
	uint32		colid;			/* DXL column id */
	Oid			type;
	int32		typmod;
	const char *name;
} OrcaPlanRegionCol;

typedef struct OrcaPlanRegion
{
	int			root;			/* operator number of the region's root */
	bool		ipc;			/* yields (segment, payload) rows */
	const char *dxl;			/* custom_private payload */
	int			nleaves;
	const int  *leaves;			/* operator numbers of the PG inputs */
	int			ncols;
	const OrcaPlanRegionCol *cols;	/* output columns */

	/*
	 * The region is a table scan without its filter and projection, which the
	 * executor evaluates over its output (the table's columns): the
	 * CustomScan's qual and target list. Its one leaf is the scan itself,
	 * which the extension reads. Not with ipc.
	 */
	bool		pg_filter;
} OrcaPlanRegion;

typedef struct OrcaPlanRegions
{
	int			version;		/* ORCA_HOOKS_ABI_VERSION */
	const struct CustomScanMethods *methods;
	int			nregions;
	const OrcaPlanRegion *regions;
} OrcaPlanRegions;

typedef const OrcaPlanRegions *(*orca_plan_regions_hook_type) (const char *plan_dxl,
															   int nops,
															   const char *const *opnames);
extern PGDLLIMPORT orca_plan_regions_hook_type orca_plan_regions_hook;

/*
 * Statistics hooks for ORCA's metadata translation, the counterparts of the
 * PG planner's get_relation_info_hook and get_relation_stats_hook for tables
 * whose data and statistics live outside PG (such as tables an extension
 * stores itself).
 *
 * orca_relation_size_hook: the relation's row and page counts. Returns false
 * to use pg_class.
 *
 * orca_att_stats_hook: a pg_statistic tuple for the column, palloc'd (the
 * caller frees it), or NULL to read pg_statistic.
 *
 * Both run with the optimizer GUC off, as Orca isn't reentrant: queries they
 * run (through SPI) are planned by PG's planner. The size hook is called for
 * the relation's statistics and again for each column's, so it should cache
 * an answer that is costly to find.
 *
 * ORCA keeps their answers with the rest of the relation's metadata in its
 * metadata cache, until an invalidation of the relation's relcache entry,
 * except for a relation orca_relation_hook says is the extension's: ORCA
 * caches none of its metadata, and calls the hooks in each query reading it.
 */
typedef bool (*orca_relation_size_hook_type) (Oid relid, double *tuples,
											  BlockNumber *pages);
extern PGDLLIMPORT orca_relation_size_hook_type orca_relation_size_hook;

typedef HeapTuple (*orca_att_stats_hook_type) (Oid relid, AttrNumber attnum);
extern PGDLLIMPORT orca_att_stats_hook_type orca_att_stats_hook;

/*
 * orca_relation_hook: whether an extension reads the relation itself, for a
 * table of a table AM ORCA doesn't otherwise plan (such as one whose data
 * lives outside PG).
 *
 * ORCA plans queries reading an ORCA_RELATION_RANDOM table as if it were a
 * randomly distributed heap table: each segment reads a slice of it. The
 * extension must execute every scan of it, as a leaf of a region it claims
 * through orca_plan_regions_hook, since PG's executor can't: otherwise the
 * query falls back to the PG planner. As its metadata isn't cached, the hook
 * may change its answer for a table between queries. ORCA considers no
 * indexes of such a table, and a partitioned table only if all of its
 * partitions are what the hook says its root is. The PG planner plans DML
 * on it, and queries over an ORCA_RELATION_PLANNER table.
 *
 * It is called several times per relation and query, so it should be cheap.
 */
typedef enum OrcaRelation
{
	ORCA_RELATION_NONE,			/* not the extension's */
	ORCA_RELATION_PLANNER,		/* the extension's, planned by PG's planner */
	ORCA_RELATION_RANDOM		/* the extension's, read in random slices */
} OrcaRelation;

typedef OrcaRelation (*orca_relation_hook_type) (Oid relid);
extern PGDLLIMPORT orca_relation_hook_type orca_relation_hook;

#endif

#endif /* ORCA_H */
