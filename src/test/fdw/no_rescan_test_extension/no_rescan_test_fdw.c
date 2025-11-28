/*
 * no_rescan_test_fdw.c
 *
 * Test FDW that intentionally does NOT implement ReScanForeignScan
 * to demonstrate automatic materialization by the planner.
 *
 * This FDW generates simple test data (id, data) and can be used
 * in join queries to verify that the planner automatically inserts
 * a Material node when rescanning is required.
 */

#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_type.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "nodes/pg_list.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/*
 * FDW-specific information for a foreign table.
 */
typedef struct NoRescanFdwPlanState
{
	int			num_rows;		/* Number of rows to generate */
} NoRescanFdwPlanState;

/*
 * Execution state for a foreign scan.
 */
typedef struct NoRescanFdwExecState
{
	int			current_row;	/* Current row number (0-based) */
	int			max_rows;		/* Maximum number of rows to generate */
	bool		scan_started;	/* Has scan been started? */
} NoRescanFdwExecState;

/* FDW callback functions */
PG_FUNCTION_INFO_V1(no_rescan_test_fdw_handler);

static void noRescanGetForeignRelSize(PlannerInfo *root,
									  RelOptInfo *baserel,
									  Oid foreigntableid);
static void noRescanGetForeignPaths(PlannerInfo *root,
									RelOptInfo *baserel,
									Oid foreigntableid);
static ForeignScan *noRescanGetForeignPlan(PlannerInfo *root,
										   RelOptInfo *baserel,
										   Oid foreigntableid,
										   ForeignPath *best_path,
										   List *tlist,
										   List *scan_clauses,
										   Plan *outer_plan);
static void noRescanBeginForeignScan(ForeignScanState *node,
									 int eflags);
static TupleTableSlot *noRescanIterateForeignScan(ForeignScanState *node);
static void noRescanEndForeignScan(ForeignScanState *node);

/* Note: We intentionally DO NOT implement ReScanForeignScan */

/*
 * Foreign-data wrapper handler function
 */
Datum
no_rescan_test_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	/* Mandatory planning functions */
	routine->GetForeignRelSize = noRescanGetForeignRelSize;
	routine->GetForeignPaths = noRescanGetForeignPaths;
	routine->GetForeignPlan = noRescanGetForeignPlan;

	/* Mandatory execution functions */
	routine->BeginForeignScan = noRescanBeginForeignScan;
	routine->IterateForeignScan = noRescanIterateForeignScan;
	routine->EndForeignScan = noRescanEndForeignScan;

	/*
	 * CRITICAL: We intentionally leave ReScanForeignScan as NULL.
	 * This demonstrates that the planner will automatically insert
	 * a Material node when this FDW is used in scenarios requiring
	 * rescanning (e.g., nested loop joins).
	 */
	routine->ReScanForeignScan = NULL;

	PG_RETURN_POINTER(routine);
}

/*
 * Estimate relation size and cost.
 */
static void
noRescanGetForeignRelSize(PlannerInfo *root,
						  RelOptInfo *baserel,
						  Oid foreigntableid)
{
	NoRescanFdwPlanState *fdw_private;

	/*
	 * For simplicity, we'll generate 10 rows.
	 * In a real FDW, you might read this from table options.
	 */
	fdw_private = (NoRescanFdwPlanState *) palloc0(sizeof(NoRescanFdwPlanState));
	fdw_private->num_rows = 10;

	baserel->rows = fdw_private->num_rows;
	baserel->fdw_private = (void *) fdw_private;

	elog(DEBUG1, "no_rescan_test_fdw: GetForeignRelSize estimated %d rows",
		 fdw_private->num_rows);
}

/*
 * Create possible access paths.
 */
static void
noRescanGetForeignPaths(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid)
{
	Cost		startup_cost = 10;
	Cost		total_cost = startup_cost + (baserel->rows * 0.01);
	ForeignPath *path;

	/*
	 * Create a simple non-parameterized ForeignPath.
	 *
	 * The key point: because we don't implement ReScanForeignScan,
	 * create_foreignscan_path will set path.rescannable = false.
	 * This allows the planner to automatically insert Material nodes
	 * for non-parameterized rescans (e.g., inner side of nested loops).
	 *
	 * For parameterized paths (required_outer != NULL), create_foreignscan_path
	 * will reject the path if we don't support rescan. This prevents generating
	 * plans that would fail at execution time in regular joins.
	 *
	 * However, correlated subqueries (SubPlans) are a special case: they are
	 * planned independently and the foreign table doesn't know it will be used
	 * in a SubPlan that requires rescanning. These will fail at execution time
	 * with "ERROR: foreign-data wrapper does not support ReScan".
	 */
	path = create_foreignscan_path(root, baserel,
								   NULL,	/* default pathtarget */
								   baserel->rows,
								   startup_cost,
								   total_cost,
								   NIL,		/* no pathkeys */
								   NULL,	/* no required_outer */
								   NULL,	/* no fdw_outerpath */
								   NIL);	/* no fdw_private */

	if (path != NULL)
	{
		add_path(baserel, (Path *) path);

		elog(DEBUG1, "no_rescan_test_fdw: Added foreign path (rescannable=%d)",
			 path->path.rescannable);
	}
}

/*
 * Create a ForeignScan plan node.
 */
static ForeignScan *
noRescanGetForeignPlan(PlannerInfo *root,
					   RelOptInfo *baserel,
					   Oid foreigntableid,
					   ForeignPath *best_path,
					   List *tlist,
					   List *scan_clauses,
					   Plan *outer_plan)
{
	NoRescanFdwPlanState *fdw_private = (NoRescanFdwPlanState *) baserel->fdw_private;
	List	   *fdw_private_list;

	/* Extract non-FDW clauses */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Pass the number of rows to execution state via fdw_private */
	fdw_private_list = list_make1_int(fdw_private->num_rows);

	/* Create the ForeignScan node */
	return make_foreignscan(tlist,
							scan_clauses,
							baserel->relid,
							NIL,	/* no fdw_exprs */
							fdw_private_list,
							NIL,	/* no fdw_scan_tlist */
							NIL,	/* no fdw_recheck_quals */
							outer_plan);
}

/*
 * Begin executing a foreign scan.
 */
static void
noRescanBeginForeignScan(ForeignScanState *node,
						 int eflags)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	NoRescanFdwExecState *exec_state;
	int			num_rows;

	/* Extract the number of rows from fdw_private */
	if (plan->fdw_private != NIL)
		num_rows = linitial_int(plan->fdw_private);
	else
		num_rows = 10;	/* default */

	/* Initialize execution state */
	exec_state = (NoRescanFdwExecState *) palloc0(sizeof(NoRescanFdwExecState));
	exec_state->current_row = 0;
	exec_state->max_rows = num_rows;
	exec_state->scan_started = true;

	node->fdw_state = (void *) exec_state;

	elog(NOTICE, "no_rescan_test_fdw: BeginForeignScan - will generate %d rows",
		 num_rows);
}

/*
 * Iterate and return the next tuple.
 */
static TupleTableSlot *
noRescanIterateForeignScan(ForeignScanState *node)
{
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	NoRescanFdwExecState *exec_state = (NoRescanFdwExecState *) node->fdw_state;

	/* Clear the slot */
	ExecClearTuple(slot);

	/* Generate rows until we reach max_rows */
	if (exec_state->current_row < exec_state->max_rows)
	{
		int			row_id = exec_state->current_row + 1;

		/*
		 * Generate simple test data:
		 * - Column 1: integer id (1, 2, 3, ...)
		 * - Column 2: text data ("row_1", "row_2", ...)
		 *
		 * Fill values/isnull arrays directly in the slot
		 */
		slot->tts_values[0] = Int32GetDatum(row_id);
		slot->tts_isnull[0] = false;

		slot->tts_values[1] = CStringGetTextDatum(psprintf("row_%d", row_id));
		slot->tts_isnull[1] = false;

		exec_state->current_row++;

		/* Store the virtual tuple in the slot */
		ExecStoreVirtualTuple(slot);
	}

	return slot;
}

/*
 * End a foreign scan.
 */
static void
noRescanEndForeignScan(ForeignScanState *node)
{
	NoRescanFdwExecState *exec_state = (NoRescanFdwExecState *) node->fdw_state;

	if (exec_state)
	{
		elog(NOTICE, "no_rescan_test_fdw: EndForeignScan - scanned %d of %d rows",
			 exec_state->current_row, exec_state->max_rows);
		pfree(exec_state);
	}
}

/*
 * Note: We deliberately DO NOT implement ReScanForeignScan.
 * This is the whole point of this test FDW - to demonstrate that
 * the planner will automatically insert a Material node when
 * rescanning is required.
 *
 * If you uncomment the following and add it to the FdwRoutine,
 * you'll see that Material nodes are no longer inserted:
 *
 * static void
 * noRescanReScanForeignScan(ForeignScanState *node)
 * {
 *     NoRescanFdwExecState *exec_state = (NoRescanFdwExecState *) node->fdw_state;
 *     exec_state->current_row = 0;
 *     elog(NOTICE, "no_rescan_test_fdw: ReScan called");
 * }
 */
