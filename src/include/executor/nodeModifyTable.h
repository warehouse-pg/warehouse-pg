/*-------------------------------------------------------------------------
 *
 * nodeModifyTable.h
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeModifyTable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMODIFYTABLE_H
#define NODEMODIFYTABLE_H

#include "nodes/execnodes.h"

extern void ExecComputeStoredGenerated(EState *estate, TupleTableSlot *slot);

extern ModifyTableState *ExecInitModifyTable(ModifyTable *node, EState *estate, int eflags);
extern void ExecEndModifyTable(ModifyTableState *node);
extern void ExecReScanModifyTable(ModifyTableState *node);
extern void ExecSquelchModifyTable(ModifyTableState *node);

/* GPDB: refuse a row that a BEFORE ROW trigger left belonging to another segment */
extern bool ExecDistKeyMisplaced(ResultRelInfo *resultRelInfo,
								 EState *estate,
								 TupleTableSlot *slot,
								 int *target_seg);
extern void ExecReportDistKeyMisplaced(ResultRelInfo *resultRelInfo,
									   const char *cmdname,
									   int target_seg,
									   int sqlerrcode);

#endif							/* NODEMODIFYTABLE_H */
