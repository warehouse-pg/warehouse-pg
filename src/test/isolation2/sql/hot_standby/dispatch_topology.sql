-- Tests for whpg_dispatch_topology_file on its real deployment shape: a
-- hot-standby dispatcher.  The standby coordinator dispatches with
-- addresses resolved from the topology file; the file's rows carry the
-- identities of the nodes a hot-standby QD actually reaches — the
-- mirrors, plus the standby coordinator itself for the entry database.
--
-- Sessions: 1: is the primary QD (writes), -1S: is a dispatch session on
-- the standby QD (everything under test), -1M: is a utility session on
-- the standby for reading the state GUC while standby dispatch is broken.
--
-- All file variants are staged up front; the GUC is set on the STANDBY
-- only (appended to its postgresql.conf + reload) — on any non-hot-standby
-- node the component-table build refuses the feature outright, which the
-- plain dispatch_topology test asserts.

-- start_matchsubs
-- m/dispatch topology file "[^"]*"/
-- s/dispatch topology file "[^"]*"/dispatch topology file "PATH"/
-- m/seg\d+ [0-9.]+:\d+/
-- s/seg\d+ [0-9.]+:\d+/segN IP:PORT/
-- m/pid=\d+/
-- s/pid=\d+/pid=PID/
-- m/line \d+:/
-- s/line \d+:/line N:/
-- m/\(cdbgang\.c:\d+\)/
-- s/\(cdbgang\.c:\d+\)/(cdbgang.c:NNN)/
-- m/Dispatched for dbid \d+, but this segment is dbid \d+\./
-- s/Dispatched for dbid \d+, but this segment is dbid \d+\./Dispatched for dbid X, but this segment is dbid Y./
-- m/Dispatched for content -?\d+, but this segment is content -?\d+\./
-- s/Dispatched for content -?\d+, but this segment is content -?\d+\./Dispatched for content X, but this segment is content Y./
-- m/duplicate dbid \d+/
-- s/duplicate dbid \d+/duplicate dbid N/
-- m/coordinator row carries dbid \d+, but this dispatcher is dbid \d+/
-- s/coordinator row carries dbid \d+, but this dispatcher is dbid \d+/coordinator row carries dbid X, but this dispatcher is dbid Y/
-- end_matchsubs

-- Stage the variants.  The good file lists the role='m' rows: the mirrors
-- (what a hot-standby QD dispatches to) and the standby coordinator (the
-- entry database — the file's contract is each node's OWN identity, which
-- for content -1 is the standby itself).
!\retcode psql -X -At -d postgres -c "select content, dbid, hostname, address, port, datadir from gp_segment_configuration where role='m' order by content" | tr '|' ' ' > "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_good";
!\retcode sed '/^0 /d' "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_good" > "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_nocontent0";
!\retcode echo "# variant: nocontent0" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_nocontent0";
!\retcode psql -X -At -d postgres -c "select t.content, t.dbid, o.hostname, o.address, o.port, t.datadir from gp_segment_configuration t join gp_segment_configuration o on o.role='m' and o.content = case t.content when 0 then 1 when 1 then 0 else t.content end where t.role='m' order by t.content" | tr '|' ' ' > "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_swapped";
!\retcode echo "# variant: swapped" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_swapped";
!\retcode psql -X -At -d postgres -c "select t.content, o.dbid, o.hostname, o.address, o.port, o.datadir from gp_segment_configuration t join gp_segment_configuration o on o.role='m' and o.content = case t.content when 0 then 1 when 1 then 0 else t.content end where t.role='m' order by t.content" | tr '|' ' ' > "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_misassigned";
!\retcode echo "# variant: misassigned" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_misassigned";
!\retcode awk 'NR==1 { saved=$2 } $1=="1" { $2=saved } { print }' "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_good" > "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_dupdbid";
!\retcode echo "# variant: dupdbid" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_dupdbid";
!\retcode { cat "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_good"; echo "65537 4093 localhost localhost 12345 /tmp/nowhere"; } > "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_overflow";
!\retcode echo "# variant: overflow" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_overflow";
!\retcode awk '$1=="0" { $3="a-hostname-longer-than-maxhostnamelen-which-is-sixty-four-on-linux-boxes" } { print }' "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_good" > "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_hostlong";
!\retcode echo "# variant: hostlong" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_hostlong";
!\retcode awk '{ if ($1=="-1") $2=1; print }' "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_good" > "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_coordwrong";
!\retcode echo "# variant: coordwrong" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_coordwrong";
!\retcode echo "garbage line" > "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_garbage";

-- Payload table, written on the primary; remote_apply (suite setup) makes
-- it visible on the standby.
create table hs_topo_t(a int) distributed by (a);
insert into hs_topo_t select generate_series(1, 100);

-- Feature off on the standby: state shows inactive.
-1S: show whpg_dispatch_topology_state;

-- Enable on the STANDBY only.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_good" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
!\retcode echo "whpg_dispatch_topology_file = 'whpg_dr_topology_test'" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf";
!\retcode pg_ctl reload -D "$COORDINATOR_DATA_DIRECTORY/../../standby";

-- Happy path: standby dispatch works from the file; state agrees from
-- both the dispatch and the utility view.
-1S: select count(*) from hs_topo_t;
-1S: select count(distinct gp_segment_id) > 1 from hs_topo_t;
-1S: show whpg_dispatch_topology_state;
-1M: show whpg_dispatch_topology_state;

-- One row per content: fail closed, no silent fallback to the catalog.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_nocontent0" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;
-- The state GUC's scope is deliberately parse-level: a catalog-level
-- crosscheck failure surfaces through the failing queries themselves
-- (above), not through this GUC — the file itself is well-formed.
-1M: show whpg_dispatch_topology_state;

-- Recovery is automatic once a good file is back.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_good" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;

-- The dbid handshake: addresses crossed, dbids kept — every connection
-- reaches a live node whose own dbid differs. Refused, permanently.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_swapped" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;

-- The content handshake: dbids AND addresses crossed — internally
-- consistent, so the dbid check passes by construction; the content half
-- refuses running one content's slices against another's data.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_misassigned" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;

-- Two rows sharing a dbid would make the handshake vacuous for one of
-- them: refused at parse.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_dupdbid" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;

-- A content beyond int16 would truncate into an alias of a real content:
-- refused at parse.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_overflow" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;

-- A hostname beyond the component table's limit is refused at parse with
-- a real message, instead of dying later inside the build with an
-- internal one.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_hostlong" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;

-- The coordinator row must carry THIS dispatcher's own identity; a row
-- carrying some other node's dbid (e.g. regenerated from a replayed
-- catalog after a source-side coordinator failover) is refused at load.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_coordwrong" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;

-- Unparsable garbage: dispatch refuses, and the state GUC agrees from the
-- utility view (parse-level failures ARE its scope).
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_garbage" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;
-1M: show whpg_dispatch_topology_state;

-- Good file again: everything recovers, both views agree.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/../../standby/topo_hs_good" "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test";
-1S: select count(*) from hs_topo_t;
-1S: show whpg_dispatch_topology_state;
-1M: show whpg_dispatch_topology_state;

-- Disable and clean up.
!\retcode sed -i '/whpg_dispatch_topology_file/d' "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf";
!\retcode pg_ctl reload -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
-1S: select count(*) from hs_topo_t;
-1S: show whpg_dispatch_topology_state;
drop table hs_topo_t;
!\retcode rm -f "$COORDINATOR_DATA_DIRECTORY/../../standby/whpg_dr_topology_test" "$COORDINATOR_DATA_DIRECTORY/../../standby"/topo_hs_*;
