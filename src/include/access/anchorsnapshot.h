/*-------------------------------------------------------------------------
 *
 * anchorsnapshot.h
 *	  Anchor snapshots: node-local xid snapshots exported by the startup
 *	  process at every replayed restore point on a hot standby, kept in a
 *	  fixed-capacity shared-memory registry and persisted under
 *	  pg_anchor_snapshots/.  Dispatch-role backends on that standby import
 *	  the anchor named by whpg_hot_standby_anchor_name as their snapshot.
 *
 * Backend import (the installer, called from the per-statement snapshot
 * funnel in snapmgr.c):
 *
 * - Applies to a dispatch-role session (Gp_role == GP_ROLE_DISPATCH) when
 *   whpg_hot_standby_snapshot_mode is anchored (the server default is
 *   unanchored; a read replica's configuration file sets anchored, and any
 *   session may SET it), the server is in recovery and the registry is
 *   enabled (whpg_max_anchor_snapshots > 0), and the session has finished
 *   initialization.  Outside recovery, or with the registry disabled, the
 *   anchored GUCs are inert and the session takes ordinary snapshots.
 *   Utility-mode sessions are never anchored.
 * - Executors are anchored by the dispatch, not by a GUC: the dispatcher
 *   ships the anchor's name in the transaction context of every statement
 *   whose snapshot carries an anchor (GP_OPT_ANCHORED_SNAPSHOT), and every
 *   executor backend serving it (writer, reader, cursor reader, the single
 *   segment of a direct dispatch, the entry-db singleton) installs ITS OWN
 *   node's anchor of that name, whatever its transaction context.  The
 *   snapshot records the registration it was installed from
 *   (SnapshotData.anchorOrdinal), so a statement's later dispatches (an
 *   initplan, a cursor's gangs) name the anchor of the snapshot they
 *   ship, and a retired anchor is refused before dispatch.  A dispatch
 *   without a snapshot carries no anchor; such dispatches read no table
 *   data.
 * - The snapshot is taken by GetSnapshotData() as usual and the anchor's
 *   xid set (xmin, xmax, the known xids, the overflow flag) is laid over
 *   its local half.  On the dispatcher the distributed half stays as
 *   taken and is shipped as usual (it selects the executors' transaction
 *   contexts and advances their distributed-log horizon); on an executor
 *   it is cleared once GetSnapshotData() has used it, so that visibility
 *   under an anchor is single-layer: the anchor's local xid set decides,
 *   the distributed log is never consulted.  The session's xmin
 *   (MyPgXact->xmin, TransactionXmin and the recent-xmin globals) is
 *   lowered to the anchor's, which is what makes replayed cleanup conflict
 *   with the reader instead of removing what it reads; readers, which
 *   publish no xmin of their own otherwise, publish it too.  The lowering
 *   happens under ProcArrayLock (shared) after re-checking that the
 *   anchor is still registered; code that invalidates an anchor must
 *   therefore delete the entry first and take ProcArrayLock exclusively
 *   once before collecting conflicting readers, so that every installer
 *   that saw the entry has published its xmin by then.
 * - An executor writer publishes its snapshot to the reader gang only
 *   after the anchor is laid over it (GetSnapshotData() skips its usual
 *   publication under an anchored dispatch), so a reader never copies the
 *   replay-position set; readers then install the same anchor from their
 *   own file.  The shared slot and the cursor dump carry
 *   takenDuringRecovery, which a recovery-shaped snapshot needs to search
 *   the right xid array.
 * - READ COMMITTED sessions install the anchor the GUC names at every
 *   statement: they see one anchor until a publication moves it.  A
 *   REPEATABLE READ transaction pins the anchor its first snapshot
 *   installed; later statements only re-check that the pinned anchor is
 *   still registered and fail when a publication retired it.  The pin
 *   outranks the mode GUC for the rest of the transaction, and a
 *   REPEATABLE READ transaction whose first snapshot was not anchored
 *   stays unanchored throughout: a mode change takes effect at the next
 *   transaction there, at the next statement in READ COMMITTED.
 * - Catalog snapshots are not anchored: a query reads table data as of
 *   the anchor and the catalog as of the replay position.
 * - Every refusal is an ERROR with SQLSTATE 55000 (object not in
 *   prerequisite state) and a message naming the anchor.  A session to
 *   which anchored reads apply never falls back to an unanchored snapshot
 *   silently; sessions to which they do not apply (utility mode, a server
 *   out of recovery, the registry disabled) take ordinary snapshots, as
 *   the GUC's description says.
 * - An anchor's identity is its registration, not its name: names are
 *   not unique on the primary and retirement frees them, so a later
 *   restore point of the same name can register again, possibly with the
 *   same xmin.  The installer therefore keys its cache and a transaction's
 *   pin on the registry's registration ordinal.
 * - After a restart an overflowed anchor (sof:1) whose xid range reaches
 *   past the start checkpoint's oldest active xid is not re-registered:
 *   StartupSUBTRANS zeroed the pg_subtrans pages its readers would map
 *   subtransactions through, and the assignment records that filled them
 *   lie before the redo start point.
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
#include "utils/snapshot.h"

/* The data-directory subdirectory holding the snapshot files. */
#define ANCHOR_SNAPSHOT_DIR "pg_anchor_snapshots"

/* GUCs; definitions live in guc_gp.c's tables */
extern int	whpg_max_anchor_snapshots;
extern char *whpg_hot_standby_anchor_name;
extern int	whpg_hot_standby_snapshot_mode;

/* Values of whpg_hot_standby_snapshot_mode */
typedef enum WhpgSnapshotMode
{
	WHPG_SNAPSHOT_MODE_ANCHORED,	/* import the published anchor */
	WHPG_SNAPSHOT_MODE_UNANCHORED	/* snapshots at the replay position (default) */
} WhpgSnapshotMode;

/* Shared memory (ipci.c) */
extern Size AnchorRegistryShmemSize(void);
extern void AnchorRegistryShmemInit(void);

/*
 * Startup process entry points (xlog.c, startup.c).  Startup takes this
 * start's redo start point (invalid outside recovery) and the recovery
 * timeline history: the anchor the GUC names is registered at once only
 * when its record is already applied, otherwise when replay meets it.
 */
extern void AnchorSnapshotStartup(XLogRecPtr redoStart, List *history,
								  TransactionId oldestActiveXid);
extern void AnchorSnapshotExportOnRestorePoint(XLogReaderState *record);
extern void AnchorSnapshotOnConfigReload(void);
extern void AnchorSnapshotClearAll(void);

/* Primitive shared with the conflict-linkage code */
extern void AnchorSnapshotInvalidate(const char *rp_name);

/*
 * Lookup for backends (import path); false when no valid entry exists.
 * ordinal (may be NULL) receives the entry's registration ordinal, the
 * identity of this registration of the name.
 */
extern bool AnchorSnapshotLookup(const char *rp_name, TransactionId *xmin,
								 uint64 *ordinal);

/*
 * Dispatch wire (cdbdtxcontextinfo.c).  AnchorSnapshotNameForDispatch
 * copies the name of the registered anchor with that ordinal into name
 * (MAXFNAMELEN bytes) or errors when it is no longer registered;
 * AnchorSnapshotDispatched says whether this executor serves an anchored
 * dispatch; GetSnapshotData() then hands the writer's slot publication to
 * the installer through AnchorSnapshotSetDeferredPublication(true) and
 * clears the hand-off at its next call; AnchorSnapshotNameIsValid
 * validates a name received from a dispatch.
 */
extern void AnchorSnapshotNameForDispatch(uint64 ordinal, char *name);
extern bool AnchorSnapshotDispatched(void);
extern void AnchorSnapshotSetDeferredPublication(bool deferred);
extern bool AnchorSnapshotNameIsValid(const char *name);

/*
 * Backend import (snapmgr.c).  AnchorSnapshotInstall lays the session's
 * anchor over a snapshot GetSnapshotData() just took and lowers the
 * session's xmin to it; with pin set, a snapshot that is to serve the
 * whole transaction pins the anchor.  It returns false, leaving the
 * snapshot alone, when anchored reads do not apply to this session.  In an
 * executor it installs the dispatched anchor instead (pin is ignored).
 * AnchorSnapshotValidatePinned re-checks a pinned anchor for the next
 * statement of the transaction; AnchorSnapshotSessionAnchored says
 * whether anchored reads apply to this session right now.
 */
extern bool AnchorSnapshotInstall(Snapshot snapshot, bool pin);
extern void AnchorSnapshotValidatePinned(void);
extern bool AnchorSnapshotTransactionPinned(void);
extern bool AnchorSnapshotSessionAnchored(void);
extern void AtEOXact_AnchorSnapshot(void);

/*
 * Horizon for pg_subtrans truncation on a standby (xlog.c): the oldest
 * xmin of any registered anchor, or InvalidTransactionId.  An anchor is an
 * importable old xmin that no backend holds between its export and its
 * import, so the subtrans pages its readers may consult must outlive that
 * gap.  Read it BEFORE GetOldestXmin(): an entry gone by then was
 * invalidated after every installer that saw it published its xmin under
 * ProcArrayLock, which GetOldestXmin then observes.
 */
extern TransactionId AnchorSnapshotOldestXmin(void);

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
