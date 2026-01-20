/*-------------------------------------------------------------------------
 *
 * sharedsnapshot.h
 *	  GPDB shared snapshot management
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sharedsnapshot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHAREDSNAPSHOT_H
#define SHAREDSNAPSHOT_H

#include "storage/proc.h"
#include "utils/combocid.h"
#include "utils/snapshot.h"

typedef struct SnapshotDump
{
	uint32 segmateSync;
	DistributedTransactionId distributedXid;
	FullTransactionId localXid;
	dsm_handle  handle;
	dsm_segment *segment;
} SnapshotDump;

#define SNAPSHOTDUMPARRAYSZ 32

/* Only hold subXIPs via DSM for large counts */
#define SUBXIPARRAY_THRESHOLD 1024

/* MPP Shared Snapshot */
typedef struct SharedSnapshotSlot
{
	int32			slotindex;  /* where in the array this one is. */
	int32	 		slotid;
	PGPROC			*writer_proc;
	PGXACT			*writer_xact;

	/* only used by cursor dump identification, dose not always set */
	volatile DistributedTransactionId distributedXid;

	volatile bool			ready;
	volatile uint32			segmateSync;
	SnapshotData	snapshot;
	LWLock		   *slotLock;

	volatile int    cur_dump_id;
	volatile SnapshotDump    dump[SNAPSHOTDUMPARRAYSZ];
	/* for debugging only */
	FullTransactionId	fullXid;
	TimestampTz		startTimestamp;
	/*
	 * It's impractical to store the maximum volume of **possible** subXIPs in
	 * the shared memory. Instead, we allocate a DSM segment to hold the subXIP
	 * array.
	 */
	volatile dsm_handle		sub_xips_handle;
} SharedSnapshotSlot;

extern volatile SharedSnapshotSlot *SharedLocalSnapshotSlot;

/* MPP Shared Snapshot */
extern Size SharedSnapshotShmemSize(void);
extern void CreateSharedSnapshotArray(void);
extern char *SharedSnapshotDump(void);

extern void SharedSnapshotDetach(volatile SharedSnapshotSlot *slot, char *readerDescription);
extern void SharedSnapshotRemove(volatile SharedSnapshotSlot *slot, char *creatorDescription);
extern void addSharedSnapshot(char *creatorDescription, int id);
extern void lookupSharedSnapshot(char *lookerDescription, char *creatorDescription, int id);

extern void dumpSharedLocalSnapshot_forCursor(void);
extern void readSharedLocalSnapshot_forCursor(Snapshot snapshot, DtxContext distributedTransactionContext);

extern void AtEOXact_SharedSnapshot(void);

#define NUM_SHARED_SNAPSHOT_SLOTS (2 * MaxBackends)

#endif   /* SHAREDSNAPSHOT_H */
