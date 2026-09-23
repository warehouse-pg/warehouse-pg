/*-------------------------------------------------------------------------
 *
 * cdbhistory.c
 *    Query history writer: shmem queue and background worker for
 *    persisting per-node execution metrics into
 *    pg_catalog.whpg_query_history at the end of each query.
 *
 * Architecture:
 *
 *   ExecutorEnd (QD only, whpg_query_history_enabled=on):
 *     -> WhpgEmitQueryHistory() walks the PlanState tree, builds one
 *        WhpgHistEntry per plan node, and enqueues them into the lock-
 *        protected ring buffer WhpgHistQ.  After enqueuing, the bgworker
 *        latch is set to wake the writer.
 *
 *   WhpgHistWriterMain (background worker):
 *     Drains the ring buffer and INSERTs rows into
 *     pg_catalog.whpg_query_history using SPI.
 *
 * Copyright (c) 2024-Present, EnterpriseDB Corporation.
 *
 * src/backend/cdb/cdbhistory.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "catalog/whpg_query_history.h"
#include "cdb/cdbhistory.h"
#include "cdb/cdblivequery.h"
#include "cdb/cdbvars.h"
#include "executor/execdesc.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/* ----------------------------------------------------------------
 * GUC variables (definitions)
 * ----------------------------------------------------------------
 */
bool	whpg_query_history_enabled = true;
int		whpg_query_history_min_duration_ms = 0;
int		whpg_history_enqueue_timeout_ms = 50;

/* ----------------------------------------------------------------
 * Shmem ring buffer
 * ----------------------------------------------------------------
 */
#define WHPG_HIST_QUEUE_CAPACITY	16384	/* must be power of 2 */
#define WHPG_HIST_QUEUE_MASK		(WHPG_HIST_QUEUE_CAPACITY - 1)

typedef struct WhpgHistQueueHeader
{
	slock_t		lock;
	uint64		head;			/* next write slot */
	uint64		tail;			/* next read slot */
	uint64		n_dropped;
	Latch	   *bgworker_latch; /* set to wake bgworker */
} WhpgHistQueueHeader;

typedef struct WhpgHistQueue
{
	WhpgHistQueueHeader hdr;
	WhpgHistEntry		entries[WHPG_HIST_QUEUE_CAPACITY];
} WhpgHistQueue;

static WhpgHistQueue *WhpgHistQ = NULL;

/* ----------------------------------------------------------------
 * Shmem sizing and initialisation
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 * Signal handling for the bgworker
 * ----------------------------------------------------------------
 */
static volatile sig_atomic_t whpg_hist_got_sigterm = false;

static void
whpg_hist_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	whpg_hist_got_sigterm = true;
	if (MyLatch != NULL)
		SetLatch(MyLatch);

	errno = save_errno;
}

Size
WhpgHistShmemSize(void)
{
	/*
	 * Always allocate the queue even when whpg_query_history_enabled=off at
	 * startup; the GUC is PGC_SIGHUP and can be toggled at runtime.  We
	 * always size the queue regardless so we never need to worry about the
	 * pointer being NULL after a reload.
	 */
	return sizeof(WhpgHistQueue);
}

void
WhpgHistShmemInit(void)
{
	bool	found;
	Size	size;

	size = WhpgHistShmemSize();

	WhpgHistQ = (WhpgHistQueue *)
		ShmemInitStruct("WhpgHistQueue", size, &found);

	if (!found)
	{
		memset(WhpgHistQ, 0, size);
		SpinLockInit(&WhpgHistQ->hdr.lock);
		WhpgHistQ->hdr.head = 0;
		WhpgHistQ->hdr.tail = 0;
		WhpgHistQ->hdr.n_dropped = 0;
		WhpgHistQ->hdr.bgworker_latch = NULL;
	}
}

/* ----------------------------------------------------------------
 * enqueue_hist_entry
 *
 * Appends one entry to the ring buffer.  Drops the entry and logs a
 * WARNING if the buffer is full.
 * ----------------------------------------------------------------
 */
static void
enqueue_hist_entry(const WhpgHistEntry *entry)
{
	SpinLockAcquire(&WhpgHistQ->hdr.lock);
	if (WhpgHistQ->hdr.head - WhpgHistQ->hdr.tail >= WHPG_HIST_QUEUE_CAPACITY)
	{
		WhpgHistQ->hdr.n_dropped++;
		SpinLockRelease(&WhpgHistQ->hdr.lock);
		ereport(LOG,
				(errmsg("whpg_query_history: queue full, dropping history row")));
		return;
	}
	WhpgHistQ->entries[WhpgHistQ->hdr.head & WHPG_HIST_QUEUE_MASK] = *entry;
	WhpgHistQ->hdr.head++;
	SpinLockRelease(&WhpgHistQ->hdr.lock);
}

/* ----------------------------------------------------------------
 * collect_planstate_history
 *
 * Recursive walker over the PlanState tree.  Builds one WhpgHistEntry
 * per node and enqueues it.
 * ----------------------------------------------------------------
 */
typedef struct WhpgHistCollectCtx
{
	WhpgLiveQuerySlot  *slot;
	TimestampTz			end_time;
	char				finish_status;
} WhpgHistCollectCtx;

static void
collect_planstate_history(PlanState *ps, int parent_node_id,
						  WhpgHistCollectCtx *ctx)
{
	WhpgHistEntry		entry;
	int32				plan_node_id;
	WhpgPlanNodeSummary *pn;
	int					i;

	if (ps == NULL)
		return;

	plan_node_id = ps->plan->plan_node_id;

	/* Look up slice_id from the registry slot */
	pn = NULL;
	for (i = 0; i < ctx->slot->n_nodes; i++)
	{
		if (ctx->slot->nodes[i].plan_node_id == plan_node_id)
		{
			pn = &ctx->slot->nodes[i];
			break;
		}
	}

	memset(&entry, 0, sizeof(entry));
	entry.queryid			= ctx->slot->queryid;
	entry.session_id		= ctx->slot->session_id;
	entry.command_count		= ctx->slot->command_count;
	entry.backend_pid		= ctx->slot->backend_pid;
	entry.dbid				= ctx->slot->dbid;
	entry.userid			= ctx->slot->userid;
	entry.start_time		= ctx->slot->start_time;
	entry.end_time			= ctx->end_time;
	entry.finish_status		= ctx->finish_status;
	entry.plan_node_id		= plan_node_id;
	entry.parent_node_id	= parent_node_id;
	entry.node_type			= (uint16) nodeTag(ps->plan);
	entry.slice_id			= (pn != NULL) ? pn->slice_id : -1;
	entry.segindex			= -1;	/* aggregated coordinator view */

	if (ps->instrument != NULL)
	{
		entry.actual_rows = whpg_i64_add_sat(
			whpg_u64_to_i64_sat(ps->instrument->tuplecount),
			whpg_u64_to_i64_sat(ps->instrument->ntuples));
		entry.actual_ms = ps->instrument->total * 1000.0;
		entry.execmem_bytes_peak =
			whpg_dbl_to_i64_sat(ps->instrument->execmemused);
		entry.workmem_bytes_peak =
			whpg_dbl_to_i64_sat(ps->instrument->workmemused);
		entry.spill_bytes =
			(int64) ps->instrument->sortSpaceUsed * 1024LL;
		entry.bufusage_shared_hit =
			(int64) ps->instrument->bufusage.shared_blks_hit;
		entry.bufusage_shared_read =
			(int64) ps->instrument->bufusage.shared_blks_read;
	}
	memcpy(entry.trace_id, ctx->slot->trace_id, 16);

	enqueue_hist_entry(&entry);

	/* Standard binary children */
	collect_planstate_history(ps->lefttree, plan_node_id, ctx);
	collect_planstate_history(ps->righttree, plan_node_id, ctx);

	/* Multi-child plan types */
	switch (nodeTag(ps->plan))
	{
		case T_Append:
		{
			AppendState *astate = (AppendState *) ps;

			for (i = 0; i < astate->as_nplans; i++)
				collect_planstate_history(astate->appendplans[i],
										  plan_node_id, ctx);
			break;
		}
		case T_MergeAppend:
		{
			MergeAppendState *mstate = (MergeAppendState *) ps;

			for (i = 0; i < mstate->ms_nplans; i++)
				collect_planstate_history(mstate->mergeplans[i],
										  plan_node_id, ctx);
			break;
		}
		case T_BitmapAnd:
		{
			BitmapAndState *bstate = (BitmapAndState *) ps;

			for (i = 0; i < bstate->nplans; i++)
				collect_planstate_history(bstate->bitmapplans[i],
										  plan_node_id, ctx);
			break;
		}
		case T_BitmapOr:
		{
			BitmapOrState *bstate = (BitmapOrState *) ps;

			for (i = 0; i < bstate->nplans; i++)
				collect_planstate_history(bstate->bitmapplans[i],
										  plan_node_id, ctx);
			break;
		}
		case T_Sequence:
		{
			SequenceState *sstate = (SequenceState *) ps;

			for (i = 0; i < sstate->numSubplans; i++)
				collect_planstate_history(sstate->subplans[i],
										  plan_node_id, ctx);
			break;
		}
		default:
			break;
	}
}

/* ----------------------------------------------------------------
 * WhpgEmitQueryHistory
 *
 * Called from standard_ExecutorEnd on the QD.  Walks the plan state
 * tree and enqueues one history row per plan node into WhpgHistQ.
 * ----------------------------------------------------------------
 */
void
WhpgEmitQueryHistory(QueryDesc *queryDesc, bool is_error)
{
	WhpgLiveQuerySlot  *slot;
	TimestampTz			end_time;
	int64				duration_ms;
	WhpgHistCollectCtx	ctx;

	if (!whpg_query_history_enabled)
		return;
	if (!IsUnderPostmaster)
		return;
	if (Gp_role != GP_ROLE_DISPATCH)
		return;
	if (WhpgHistQ == NULL || WhpgRegistry == NULL)
		return;
	if (queryDesc == NULL || queryDesc->planstate == NULL)
		return;

	slot = WhpgFindActiveSlot();
	if (slot == NULL)
		return;

	end_time = GetCurrentTimestamp();

	/* Check min_duration filter */
	duration_ms = (end_time - slot->start_time) / 1000;
	if (duration_ms < (int64) whpg_query_history_min_duration_ms)
		return;

	ctx.slot			= slot;
	ctx.end_time		= end_time;
	ctx.finish_status	= is_error ? 'e' : 'o';

	/* Walk the plan state tree and enqueue one entry per node */
	collect_planstate_history(queryDesc->planstate, -1 /* root */, &ctx);

	/* Wake the bgworker */
	SpinLockAcquire(&WhpgHistQ->hdr.lock);
	if (WhpgHistQ->hdr.bgworker_latch != NULL)
		SetLatch(WhpgHistQ->hdr.bgworker_latch);
	SpinLockRelease(&WhpgHistQ->hdr.lock);
}

/* ----------------------------------------------------------------
 * WhpgHistWriterMain
 *
 * Background worker entrypoint.  Drains WhpgHistQ and INSERTs rows
 * into pg_catalog.whpg_query_history via SPI.
 * ----------------------------------------------------------------
 */
void
WhpgHistWriterMain(Datum arg)
{
	pqsignal(SIGTERM, whpg_hist_sigterm);
	BackgroundWorkerUnblockSignals();

	/*
	 * Connect immediately to the default database, following the same pattern
	 * as DtxRecoveryMain.  A BGWORKER_BACKEND_DATABASE_CONNECTION worker must
	 * not wait before connecting; doing so leaves the process in an undefined
	 * state and can cause the postmaster to panic.
	 *
	 * All history rows are written to DB_FOR_COMMON_ACCESS ("postgres").
	 * The table exists there because system_views.sql runs for every database
	 * created from template1 during initdb.
	 */
	BackgroundWorkerInitializeConnection(DB_FOR_COMMON_ACCESS, NULL, 0);

	/* Register our latch so producers can wake us */
	SpinLockAcquire(&WhpgHistQ->hdr.lock);
	WhpgHistQ->hdr.bgworker_latch = MyLatch;
	SpinLockRelease(&WhpgHistQ->hdr.lock);

	/*
	 * Main processing loop.
	 */
	for (;;)
	{
		uint64	head;
		uint64	tail;
		int		rc;

		/* Check for shutdown */
		SpinLockAcquire(&WhpgHistQ->hdr.lock);
		head = WhpgHistQ->hdr.head;
		tail = WhpgHistQ->hdr.tail;
		SpinLockRelease(&WhpgHistQ->hdr.lock);

		if (head == tail)
		{
			/* Nothing to do - wait */
			rc = WaitLatch(MyLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
						   1000L,
						   WAIT_EVENT_WHPG_HIST_WRITER);
			ResetLatch(MyLatch);

			if (whpg_hist_got_sigterm || (rc & WL_POSTMASTER_DEATH))
				proc_exit(0);

			continue;
		}

#define WHPG_HIST_BATCH_SIZE	256

		/*
		 * Process entries in batches of up to WHPG_HIST_BATCH_SIZE.
		 * Each batch runs in its own transaction wrapped by PG_TRY so
		 * that a failed INSERT aborts only the batch, not the worker.
		 * The tail is always advanced to batch_end regardless of outcome
		 * so a corrupt entry cannot stall the queue indefinitely.
		 */
		while (tail < head)
		{
			uint64	batch_end;
			uint64	t;

			batch_end = tail + WHPG_HIST_BATCH_SIZE;
			if (batch_end > head)
				batch_end = head;

			StartTransactionCommand();
			PG_TRY();
			{
				if (SPI_connect() != SPI_OK_CONNECT)
					ereport(ERROR,
							(errmsg("whpg history writer: SPI_connect failed")));

				for (t = tail; t < batch_end; t++)
				{
					WhpgHistEntry  *entry;
					Oid				argtypes[WHPG_QUERY_HISTORY_NATTS];
					Datum			values[WHPG_QUERY_HISTORY_NATTS];
					char			finish_status_str[2];
					char			node_type_str[32];
					struct varlena *trace_bytea_buf;
					int				col;

					entry = &WhpgHistQ->entries[t & WHPG_HIST_QUEUE_MASK];

					finish_status_str[0] = entry->finish_status;
					finish_status_str[1] = '\0';

					strlcpy(node_type_str,
							WhpgNodeTypeStr((NodeTag) entry->node_type),
							sizeof(node_type_str));

					trace_bytea_buf = (struct varlena *)
						palloc(VARHDRSZ + 16);
					SET_VARSIZE(trace_bytea_buf, VARHDRSZ + 16);
					memcpy(VARDATA(trace_bytea_buf), entry->trace_id, 16);

					col = 0;
					argtypes[col] = INT8OID;   values[col] = Int64GetDatum(entry->queryid);           col++;
					argtypes[col] = INT4OID;   values[col] = Int32GetDatum(entry->session_id);        col++;
					argtypes[col] = INT4OID;   values[col] = Int32GetDatum(entry->command_count);     col++;
					argtypes[col] = INT4OID;   values[col] = Int32GetDatum(entry->backend_pid);       col++;
					argtypes[col] = OIDOID;    values[col] = ObjectIdGetDatum(entry->dbid);           col++;
					argtypes[col] = OIDOID;    values[col] = ObjectIdGetDatum(entry->userid);         col++;
					argtypes[col] = TIMESTAMPTZOID; values[col] = TimestampTzGetDatum(entry->start_time); col++;
					argtypes[col] = TIMESTAMPTZOID; values[col] = TimestampTzGetDatum(entry->end_time);   col++;
					argtypes[col] = TEXTOID;   values[col] = CStringGetTextDatum(finish_status_str);  col++;
					argtypes[col] = INT4OID;   values[col] = Int32GetDatum(entry->slice_id);          col++;
					argtypes[col] = INT4OID;   values[col] = Int32GetDatum(entry->segindex);          col++;
					argtypes[col] = INT4OID;   values[col] = Int32GetDatum(entry->plan_node_id);      col++;
					argtypes[col] = INT4OID;   values[col] = Int32GetDatum(entry->parent_node_id);    col++;
					argtypes[col] = TEXTOID;   values[col] = CStringGetTextDatum(node_type_str);      col++;
					argtypes[col] = INT8OID;   values[col] = Int64GetDatum(entry->actual_rows);       col++;
					argtypes[col] = FLOAT8OID; values[col] = Float8GetDatum(entry->actual_ms);        col++;
					argtypes[col] = INT8OID;   values[col] = Int64GetDatum(entry->execmem_bytes_peak); col++;
					argtypes[col] = INT8OID;   values[col] = Int64GetDatum(entry->workmem_bytes_peak); col++;
					argtypes[col] = INT8OID;   values[col] = Int64GetDatum(entry->spill_bytes);       col++;
					argtypes[col] = INT8OID;   values[col] = Int64GetDatum(entry->bufusage_shared_hit); col++;
					argtypes[col] = INT8OID;   values[col] = Int64GetDatum(entry->bufusage_shared_read); col++;
					argtypes[col] = FLOAT8OID; values[col] = Float8GetDatum(entry->cpu_user_ms);      col++;
					argtypes[col] = FLOAT8OID; values[col] = Float8GetDatum(entry->cpu_sys_ms);       col++;
					argtypes[col] = BYTEAOID;  values[col] = PointerGetDatum(trace_bytea_buf);        col++;

					Assert(col == WHPG_QUERY_HISTORY_NATTS);

					SPI_execute_with_args(
						"INSERT INTO " WHPG_QUERY_HISTORY_TABLE
						" VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,"
						"$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,"
						"$21,$22,$23,$24)",
						WHPG_QUERY_HISTORY_NATTS,
						argtypes,
						values,
						NULL,	/* all non-null */
						false,
						0);
				}

				SPI_finish();
				CommitTransactionCommand();
			}
			PG_CATCH();
			{
				SPI_finish();
				AbortCurrentTransaction();
				ereport(WARNING,
						(errmsg("whpg history writer: batch failed, "
								"%llu rows dropped",
								(unsigned long long) (batch_end - tail))));
			}
			PG_END_TRY();

			/* Always advance tail past this batch */
			tail = batch_end;
			SpinLockAcquire(&WhpgHistQ->hdr.lock);
			WhpgHistQ->hdr.tail = tail;
			head = WhpgHistQ->hdr.head;
			SpinLockRelease(&WhpgHistQ->hdr.lock);
		}
	}
}

/* ----------------------------------------------------------------
 * WhpgHistWriterStartRule
 *
 * Called by the postmaster to decide whether to start the bgworker.
 * ----------------------------------------------------------------
 */
bool
WhpgHistWriterStartRule(Datum main_arg)
{
	(void) main_arg;
	return whpg_query_history_enabled;
}
