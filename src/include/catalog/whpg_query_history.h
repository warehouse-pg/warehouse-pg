/*-------------------------------------------------------------------------
 *
 * whpg_query_history.h
 *    Column index constants for pg_catalog.whpg_query_history.
 *
 * Copyright (c) 2024-Present, EnterpriseDB Corporation.
 *
 * src/include/catalog/whpg_query_history.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WHPG_QUERY_HISTORY_H
#define WHPG_QUERY_HISTORY_H

/* Fully qualified table name */
#define WHPG_QUERY_HISTORY_TABLE	"pg_catalog.whpg_query_history"

/* Total number of columns */
#define WHPG_QUERY_HISTORY_NATTS	24

/*
 * 1-based column index constants matching the CREATE TABLE column order.
 */
#define WHPG_QH_COL_QUERYID				1
#define WHPG_QH_COL_SESSION_ID			2
#define WHPG_QH_COL_COMMAND_COUNT		3
#define WHPG_QH_COL_BACKEND_PID		4
#define WHPG_QH_COL_DBID				5
#define WHPG_QH_COL_USERID				6
#define WHPG_QH_COL_START_TIME			7
#define WHPG_QH_COL_END_TIME			8
#define WHPG_QH_COL_FINISH_STATUS		9
#define WHPG_QH_COL_SLICE_ID			10
#define WHPG_QH_COL_SEGINDEX			11
#define WHPG_QH_COL_PLAN_NODE_ID		12
#define WHPG_QH_COL_PARENT_NODE_ID		13
#define WHPG_QH_COL_NODE_TYPE			14
#define WHPG_QH_COL_ACTUAL_ROWS			15
#define WHPG_QH_COL_ACTUAL_MS			16
#define WHPG_QH_COL_EXECMEM_BYTES_PEAK	17
#define WHPG_QH_COL_WORKMEM_BYTES_PEAK	18
#define WHPG_QH_COL_SPILL_BYTES			19
#define WHPG_QH_COL_BUFUSAGE_SHARED_HIT	20
#define WHPG_QH_COL_BUFUSAGE_SHARED_READ	21
#define WHPG_QH_COL_CPU_USER_MS			22
#define WHPG_QH_COL_CPU_SYS_MS			23
#define WHPG_QH_COL_TRACE_ID			24

#endif							/* WHPG_QUERY_HISTORY_H */
