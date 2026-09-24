/*-------------------------------------------------------------------------
 *
 * anchorsnapshot.h
 *	  Anchor snapshots: node-local xid snapshots exported by the startup
 *	  process at every replayed restore point on a hot standby, kept in a
 *	  fixed-capacity shared-memory registry and persisted under
 *	  pg_anchor_snapshots/.
 *
 * Copyright (c) 2026-Present EnterpriseDB Corporation.
 *
 * src/include/access/anchorsnapshot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANCHORSNAPSHOT_H
#define ANCHORSNAPSHOT_H

#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "nodes/pg_list.h"

/* The data-directory subdirectory holding the snapshot files. */
#define ANCHOR_SNAPSHOT_DIR "pg_anchor_snapshots"

/* GUCs; definitions live in guc_gp.c's tables */
extern int	whpg_max_anchor_snapshots;
extern char *whpg_hot_standby_anchor_name;

/* Shared memory (ipci.c) */
extern Size AnchorRegistryShmemSize(void);
extern void AnchorRegistryShmemInit(void);

/*
 * Startup process entry points (xlog.c, startup.c).  Startup takes this
 * start's redo start point (invalid outside recovery) and the recovery
 * timeline history: the anchor the GUC names is registered at once only
 * when its record is already applied, otherwise when replay meets it.
 */
extern void AnchorSnapshotStartup(XLogRecPtr redoStart, List *history);
extern void AnchorSnapshotExportOnRestorePoint(XLogReaderState *record);
extern void AnchorSnapshotOnConfigReload(void);
extern void AnchorSnapshotClearAll(void);

/* Primitive shared with the conflict-linkage code */
extern void AnchorSnapshotInvalidate(const char *rp_name);

/* Lookup for backends (import path); false when no valid entry exists */
extern bool AnchorSnapshotLookup(const char *rp_name, TransactionId *xmin);

/*
 * Introspection for gp_toolkit.whpg_anchor_snapshots(): copies the
 * registered anchors in registration order into dst (at most max) and
 * returns how many there are.  A registered anchor is one whose snapshot
 * this node holds; whether importing it is safe against replayed cleanup
 * is the conflict linkage's business.  The catalog carries no function
 * for this so that 7.x data directories keep their catalog version.
 */
typedef struct AnchorSnapshotInfo
{
	char		rp_name[MAXFNAMELEN];
	TransactionId xmin;
} AnchorSnapshotInfo;

extern int	AnchorSnapshotList(AnchorSnapshotInfo *dst, int max);

#endif							/* ANCHORSNAPSHOT_H */
