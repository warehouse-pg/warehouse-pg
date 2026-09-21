/*
 * gp_toolkit.whpg_anchor_snapshots(): the anchor snapshots registered on
 * this node.
 *
 * The registry itself lives in the server (access/anchorsnapshot.c); this
 * function only reads it.  It is exposed through gp_toolkit rather than
 * pg_catalog so that adding it does not change the catalog version of 7.x
 * data directories: an in-place minor upgrade keeps working, and a DR
 * replica restored from a production backup can run a newer minor than
 * production.  The catalog entry arrives through ALTER EXTENSION gp_toolkit
 * UPDATE on the production cluster and reaches the replica by replay.
 */
#include "postgres.h"

#include "access/anchorsnapshot.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/builtins.h"

Datum		whpg_anchor_snapshots(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(whpg_anchor_snapshots);

typedef struct
{
	AnchorSnapshotInfo *rows;
	int			nrows;
	int			next;
} AnchorSnapshotsContext;

Datum
whpg_anchor_snapshots(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	AnchorSnapshotsContext *ctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		int			max;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "rp_name", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "xmin", XIDOID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		ctx = (AnchorSnapshotsContext *) palloc0(sizeof(AnchorSnapshotsContext));
		max = Max(whpg_max_anchor_snapshots, 0);
		if (max > 0)
		{
			ctx->rows = (AnchorSnapshotInfo *)
				palloc(sizeof(AnchorSnapshotInfo) * max);
			ctx->nrows = AnchorSnapshotList(ctx->rows, max);
			if (ctx->nrows > max)
				ctx->nrows = max;
		}
		funcctx->user_fctx = ctx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	ctx = (AnchorSnapshotsContext *) funcctx->user_fctx;

	if (ctx->next < ctx->nrows)
	{
		AnchorSnapshotInfo *e = &ctx->rows[ctx->next++];
		Datum		values[2];
		bool		nulls[2] = {false, false};
		HeapTuple	tuple;

		values[0] = CStringGetTextDatum(e->rp_name);
		values[1] = TransactionIdGetDatum(e->xmin);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}
