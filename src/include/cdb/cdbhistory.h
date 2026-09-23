/*-------------------------------------------------------------------------
 *
 * cdbhistory.h
 *    Query history writer: shmem queue and bgworker for persisting
 *    per-node execution metrics into pg_catalog.whpg_query_history.
 *
 * Copyright (c) 2024-Present, EnterpriseDB Corporation.
 *
 * src/include/cdb/cdbhistory.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBHISTORY_H
#define CDBHISTORY_H

#include "postmaster/bgworker.h"
#include "executor/execdesc.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

/* ----------------------------------------------------------------
 * Per-row entry written into whpg_query_history (one row per plan node)
 * ----------------------------------------------------------------
 */
typedef struct WhpgHistEntry
{
	int64		queryid;
	int32		session_id;
	int32		command_count;
	int32		backend_pid;
	Oid			dbid;
	Oid			userid;
	TimestampTz	start_time;
	TimestampTz	end_time;
	char		finish_status;	/* 'o' = ok, 'e' = error */
	char		pad[3];			/* alignment */
	int32		slice_id;
	int32		segindex;
	int32		plan_node_id;
	int32		parent_node_id;
	uint16		node_type;
	char		pad2[6];		/* alignment padding */
	int64		actual_rows;
	float8		actual_ms;
	int64		execmem_bytes_peak;
	int64		workmem_bytes_peak;
	int64		spill_bytes;
	int64		bufusage_shared_hit;
	int64		bufusage_shared_read;
	float8		cpu_user_ms;
	float8		cpu_sys_ms;
	uint8		trace_id[16];
} WhpgHistEntry;

/* ----------------------------------------------------------------
 * GUC variables (defined in cdbhistory.c)
 * ----------------------------------------------------------------
 */
extern bool whpg_query_history_enabled;
extern int	whpg_query_history_min_duration_ms;
extern int	whpg_history_enqueue_timeout_ms;

/* ----------------------------------------------------------------
 * Shmem lifecycle API
 * ----------------------------------------------------------------
 */
extern Size WhpgHistShmemSize(void);
extern void WhpgHistShmemInit(void);

/* ----------------------------------------------------------------
 * Executor hook API (called from execMain.c)
 * ----------------------------------------------------------------
 */
extern void WhpgEmitQueryHistory(QueryDesc *queryDesc, bool is_error);

/* ----------------------------------------------------------------
 * Background worker API
 * ----------------------------------------------------------------
 */
extern void WhpgHistWriterMain(Datum arg);
extern bool WhpgHistWriterStartRule(BackgroundWorker *worker);

#endif							/* CDBHISTORY_H */
