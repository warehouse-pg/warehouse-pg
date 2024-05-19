#include "postgres.h"

#include "access/distributedlog.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "funcapi.h"
#include "utils/builtins.h"


/*
 * The original functions contain bugs caused by incorrect Dxid->Xid
 * conversion. For catalog compatibility, we cannot modify them in
 * place. Instead, the corrected versions are added in gp_toolkit
 * (gp_toolkit--1.6--1.7.sql).
 */
Datum		gp_distributed_xid_v2(PG_FUNCTION_ARGS);
Datum		gp_distributed_xacts_v2(PG_FUNCTION_ARGS);
Datum		gp_distributed_log_v2(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gp_distributed_xid_v2);
PG_FUNCTION_INFO_V1(gp_distributed_xacts_v2);
PG_FUNCTION_INFO_V1(gp_distributed_log_v2);

/* the fixed version for gp_distributed_xid() */
Datum
gp_distributed_xid_v2(PG_FUNCTION_ARGS pg_attribute_unused())
{
	DistributedTransactionId xid = getDistributedTransactionId();

	PG_RETURN_GXID(xid);
}

/* the fixed version for gp_distributed_xacts__() */
Datum
gp_distributed_xacts_v2(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	TMGALLXACTSTATUS *allDistributedXactStatus;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match gp_distributed_xacts view in system_views.sql */
		tupdesc = CreateTemplateTupleDesc(4);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "distributed_xid",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "state",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "gp_session_id",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "xmin_distributed_snapshot",
						   INT8OID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * Collect all the locking information that we will format and send
		 * out as a result set.
		 */
		getAllDistributedXactStatus(&allDistributedXactStatus);
		funcctx->user_fctx = (void *) allDistributedXactStatus;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	allDistributedXactStatus = (TMGALLXACTSTATUS *) funcctx->user_fctx;

	while (true)
	{
		TMGXACTSTATUS *distributedXactStatus;

		Datum		values[6];
		bool		nulls[6];
		HeapTuple	tuple;
		Datum		result;

		if (!getNextDistributedXactStatus(allDistributedXactStatus,
										  &distributedXactStatus))
			break;

		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, false, sizeof(nulls));

		values[0] = DistributedTransactionIdGetDatum(distributedXactStatus->gxid);
		values[1] = CStringGetTextDatum(DtxStateToString(distributedXactStatus->state));

		values[2] = UInt32GetDatum(distributedXactStatus->sessionId);
		values[3] = DistributedTransactionIdGetDatum(distributedXactStatus->xminDistributedSnapshot);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}

/* the fixed version for gp_distributed_log() */
Datum
gp_distributed_log_v2(PG_FUNCTION_ARGS)
{
	typedef struct Context
	{
		TransactionId		indexXid;
	} Context;

	FuncCallContext *funcctx;
	Context *context;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function
		 * calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match gp_distributed_log view in system_views.sql */
		tupdesc = CreateTemplateTupleDesc(5);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "segment_id",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "dbid",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "distributed_xid",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "status",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "local_transaction",
						   XIDOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * Collect all the locking information that we will format and send
		 * out as a result set.
		 */
		context = (Context *) palloc(sizeof(Context));
		funcctx->user_fctx = (void *) context;

		context->indexXid = XidFromFullTransactionId(ShmemVariableCache->nextFullXid);
												// Start with last possible + 1.

		funcctx->user_fctx = (void *) context;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	context = (Context *) funcctx->user_fctx;

	if (!IS_QUERY_DISPATCHER())
	{
		/*
		 * Go backwards until we don't find a distributed log page
		 */
		while (true)
		{
			DistributedTransactionId 		distribXid;
			Datum		values[6];
			bool		nulls[6];
			HeapTuple	tuple;
			Datum		result;

			if (context->indexXid < FirstNormalTransactionId)
				break;

			if (!DistributedLog_ScanForPrevCommitted(
					&context->indexXid,
					&distribXid))
				break;

			/*
			 * Form tuple with appropriate data.
			 */
			MemSet(values, 0, sizeof(values));
			MemSet(nulls, false, sizeof(nulls));

			values[0] = Int16GetDatum((int16)GpIdentity.segindex);
			values[1] = Int16GetDatum((int16)GpIdentity.dbid);
			values[2] = DistributedTransactionIdGetDatum(distribXid);

			/*
			 * For now, we only log committed distributed transactions.
			 */
			values[3] = CStringGetTextDatum("Committed");

			values[4] = TransactionIdGetDatum(context->indexXid);

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
			result = HeapTupleGetDatum(tuple);
			SRF_RETURN_NEXT(funcctx, result);
		}
	}
	SRF_RETURN_DONE(funcctx);
}

/* helper function for test: set next Gxid */
PG_FUNCTION_INFO_V1(gp_set_next_gxid);
Datum
gp_set_next_gxid(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						(errmsg("Superuser only to execute it"))));

	DistributedTransactionId new_gxid = PG_GETARG_INT64(0);

	SpinLockAcquire(shmGxidGenLock);
	ShmemVariableCache->nextGxid = new_gxid;
	ShmemVariableCache->GxidCount = 0;
	SpinLockRelease(shmGxidGenLock);

	PG_RETURN_VOID();
}
