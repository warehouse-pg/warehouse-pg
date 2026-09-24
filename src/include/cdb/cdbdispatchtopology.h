/*-------------------------------------------------------------------------
 *
 * cdbdispatchtopology.h
 *	  Resolve dispatch addresses from a topology file, bypassing
 *	  gp_segment_configuration row selection.
 *
 * When the whpg_dispatch_topology_file GUC is set on a dispatcher, the
 * component-table build replaces the rows read from the catalog with the
 * rows of the topology file: exactly one row per content.  The file can
 * never introduce a content the catalog does not know, and a content
 * without a file row is an error, never a silent fallback to the possibly
 * stale catalog.
 *
 * Misrouting protection, and its limit: connections dispatched from a
 * topology row carry the row's dbid and content, and the receiving node
 * verifies both against its own identity and demands of itself that it
 * be in recovery, entered through either signal file — standby.signal
 * (a streaming standby) or recovery.signal (a targeted-recovery replica,
 * the whpg-dr shape) — see cdbgang_parse_gpqeid_params.  The recovery
 * demand is what catches a row pointing at the corresponding node of the
 * SOURCE cluster — a live primary whose dbid and content match by clone
 * construction.  What no kernel check can catch is a row pointing at ANY
 * OTHER CLONE of the same source that is in recovery — another replica,
 * or a scratch PITR restore started on a former replica node's address:
 * clones share every stable identity (dbids, contents, system
 * identifier, timeline).  Generating the file from the replica's own
 * inventory — never from another cluster's — and retiring it before a
 * replica node's address is reused remain the tool's responsibility.
 *
 * Background-worker dormancy: while the GUC is set, the catalog's
 * addresses are not authoritative, so the dispatch-role background
 * workers on this coordinator do not act on them.  FTS skips its whole
 * cycle -- no probe, no gp_segment_configuration update, no configuration
 * dump, no status-version bump -- while its start/done counters keep
 * moving so waiters never spin; dtx recovery neither recovers nor aborts
 * prepared transactions (so *shmDtmStarted stays unset, which refuses
 * dispatch connections with "waiting for distributed transaction
 * recovery"); the global deadlock detector does not check; the autovacuum
 * launcher starts no worker (a coordinator worker runs in dispatch role
 * and would be refused at its first component-table build).  With the DTM
 * unstarted, every background worker that waits for it (the deadlock
 * detector, sweepers, extension workers) does not start at all
 * (bgworker_should_start_mpp), so the deadlock detector's own gate only
 * matters for a GUC set on a running primary.  None of them runs in
 * recovery anyway; the gate covers the window right after a promotion,
 * before the catalog has been fixed and the file retired.  Clearing the
 * GUC and reloading lifts the gate on the next cycle, and a worker that
 * cached a component table discards it then (see
 * dispatch_topology_bgworker_gated).
 *
 * Copyright (c) 2026-Present EnterpriseDB Corporation.
 *
 * src/include/cdb/cdbdispatchtopology.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBDISPATCHTOPOLOGY_H
#define CDBDISPATCHTOPOLOGY_H

typedef struct DispatchTopologyEntry
{
	int16		dbid;
	int16		content;		/* -1 for the coordinator row */
	int32		port;
	char	   *hostname;
	char	   *address;
	char	   *datadir;
} DispatchTopologyEntry;

typedef struct DispatchTopology
{
	int			nentries;		/* includes the content=-1 coordinator row */
	DispatchTopologyEntry *entries; /* sorted by content, ascending */
	char	   *signature;		/* stat identity of the parsed file, in the
								 * dispatch_topology_signature() format;
								 * taken by fstat'ing the very descriptor
								 * the parse read, never from a second look
								 * at the path */
} DispatchTopology;

/* GUC variables (guc_gp.c) */
extern char *whpg_dispatch_topology_file;
extern char *whpg_dispatch_topology_state_str;

extern bool dispatch_topology_enabled(void);
extern DispatchTopology *dispatch_topology_load(void);
extern char *dispatch_topology_signature(void);
extern bool dispatch_topology_signature_matches(const char *stored);
extern const char *show_whpg_dispatch_topology_state(void);
extern bool dispatch_topology_bgworker_gated(bool *gated, const char *worker,
											 const char *activity);
extern void dispatch_topology_bgworker_wait(long timeout_ms, uint32 wait_event);
extern void dispatch_topology_bgworker_discard_components(void);

#endif   /* CDBDISPATCHTOPOLOGY_H */
