/*-------------------------------------------------------------------------
 *
 * cdbdtxcontextinfo.h
 *
 * Portions Copyright (c) 2007-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/include/cdb/cdbdtxcontextinfo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBDTXCONTEXTINFO_H
#define CDBDTXCONTEXTINFO_H

#include "utils/snapshot.h"

/*
 * Length of an anchor snapshot name (a restore point name): MAXFNAMELEN of
 * access/xlog_internal.h, asserted equal in cdbdtxcontextinfo.c so that this
 * widely included header need not pull that one in.
 */
#define DTX_ANCHOR_NAME_LEN		64

#define DtxContextInfo_StaticInit {InvalidDistributedTransactionId,false,false,DistributedSnapshot_StaticInit,0,0,0,0,{0}}

typedef struct DtxContextInfo
{
	DistributedTransactionId 		distributedXid;
	
	bool							haveDistributedSnapshot;
	bool							cursorContext;
	
	DistributedSnapshot		 		distributedSnapshot;

	int 							distributedTxnOptions;

	uint32							segmateSync;
	uint32							nestingLevel;

	/* currentCommandId of QD, for debugging only */
	CommandId				 		curcid;	

	/*
	 * Name of the anchor snapshot the dispatched snapshot carries (WHPG hot
	 * standby, access/anchorsnapshot.h); meaningful, and on the wire, only
	 * when distributedTxnOptions has GP_OPT_ANCHORED_SNAPSHOT set.
	 */
	char							anchorName[DTX_ANCHOR_NAME_LEN];
} DtxContextInfo;

extern DtxContextInfo QEDtxContextInfo;	

extern void DtxContextInfo_Reset(DtxContextInfo *dtxContextInfo);
extern void DtxContextInfo_CreateOnCoordinator(DtxContextInfo *dtxContextInfo, bool inCursor,
										  int txnOptions, Snapshot snapshot);
extern int DtxContextInfo_SerializeSize(DtxContextInfo *dtxContextInfo);

extern void DtxContextInfo_Serialize(char *buffer, DtxContextInfo *dtxContextInfo);
extern void DtxContextInfo_Deserialize(const char *serializedDtxContextInfo,
									   int serializedDtxContextInfolen,
									   DtxContextInfo *dtxContextInfo);

extern void DtxContextInfo_Copy(DtxContextInfo *target, DtxContextInfo *source);
#endif   /* CDBDTXCONTEXTINFO_H */
