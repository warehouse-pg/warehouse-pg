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
 * A region marked ipc instead yields (segment int4, payload bytea) rows for
 * the motion right above its root, which only routes them: by the segment
 * column, or as a gather or broadcast, and neither sorts nor filters them.
 * Such a region has no columns, and the consumers of the motion must be
 * leaves of another region. Below a CTE producer, every consumer of the CTE
 * must be such a leaf, and reads the rows; below a Sequence, as its last
 * child, the Sequence passes them on, to a motion or a region.
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
 * query falls back to the PG planner. The PG planner plans DML on it, and
 * queries over an ORCA_RELATION_PLANNER table.
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
