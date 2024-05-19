/*-------------------------------------------------------------------------
 *
 * cdbdistributedxid.c
 *		Function to return maximum distributed transaction id.
 *
 * IDENTIFICATION
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "utils/builtins.h"
#include "cdb/cdbtm.h"

Datum		gp_distributed_xid(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gp_distributed_xid);

/*
 * Note: this function contains a bug caused by incorrect Dxid->Xid conversion.
 * The corrected implementation is available in gp_toolkit; see gpcontrib/gp_toolkit/gxid_func.c.
 */
Datum
gp_distributed_xid(PG_FUNCTION_ARGS pg_attribute_unused())
{
	DistributedTransactionId xid = getDistributedTransactionId();

	if (xid > UINT_MAX)
		ereport(ERROR,
				(errmsg("This function/view contains a bug: "
						"It returns incorrect result when DistributedTransactionId exceeds UINT_MAX(4294967295)"),
				errhint("Please use gp_toolkit.gp_distributed_xid as a replacement.")));

	PG_RETURN_XID(xid);

}
