-- Tests for whpg_dispatch_topology_file: dispatch resolves segment
-- addresses from a topology file, bypassing gp_segment_configuration
-- row selection.  Covers the three safety rules — exactly one file
-- row per content, the dispatched-for dbid must match the reached
-- segment's own dbid, and an invalid file refuses dispatch rather
-- than falling back — and that the feature is inert when the GUC is
-- empty.
--
-- All file variants are staged up front while the cluster is healthy,
-- because once the active file is broken, new dispatch-mode
-- connections are refused (fail closed) and the catalog cannot be
-- queried to rebuild the file.  A single session carries all queries;
-- an established session survives the errors and recovers as soon as
-- a good file is back.

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
-- end_matchsubs

-- Stage the variants: good, content-0 row missing, dbids reaching the
-- wrong live segments (addresses of contents 0 and 1 crossed), no
-- coordinator row, and unparsable garbage.
!\retcode psql -X -At -d postgres -c "select content, dbid, hostname, address, port, datadir from gp_segment_configuration where role='p' order by content" | tr '|' ' ' > "$COORDINATOR_DATA_DIRECTORY/topo_variant_good";
!\retcode sed '/^0 /d' "$COORDINATOR_DATA_DIRECTORY/topo_variant_good" > "$COORDINATOR_DATA_DIRECTORY/topo_variant_nocontent0";
!\retcode psql -X -At -d postgres -c "select t.content, t.dbid, o.hostname, o.address, o.port, t.datadir from gp_segment_configuration t join gp_segment_configuration o on o.role='p' and o.content = case t.content when 0 then 1 when 1 then 0 else t.content end where t.role='p' order by t.content" | tr '|' ' ' > "$COORDINATOR_DATA_DIRECTORY/topo_variant_swapped";
!\retcode grep -v '^-1 ' "$COORDINATOR_DATA_DIRECTORY/topo_variant_good" > "$COORDINATOR_DATA_DIRECTORY/topo_variant_nocoord";
!\retcode echo "garbage line" > "$COORDINATOR_DATA_DIRECTORY/topo_variant_garbage";

-- Feature off: state shows inactive.
1: show whpg_dispatch_topology_state;

!\retcode cp "$COORDINATOR_DATA_DIRECTORY/topo_variant_good" "$COORDINATOR_DATA_DIRECTORY/whpg_dr_topology_test";
!\retcode gpconfig -c whpg_dispatch_topology_file -v whpg_dr_topology_test --coordinatoronly --skipvalidation;
!\retcode gpstop -u;

-- Happy path: dispatch works from the file, state shows active.
1: create table dispatch_topology_t(a int) distributed by (a);
1: insert into dispatch_topology_t select generate_series(1, 100);
1: select count(*) from dispatch_topology_t;
1: select count(distinct gp_segment_id) > 1 from dispatch_topology_t;
1: show whpg_dispatch_topology_state;

-- One row per content: a content without a file row fails the
-- query; no silent fallback to the catalog.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/topo_variant_nocontent0" "$COORDINATOR_DATA_DIRECTORY/whpg_dr_topology_test";
1: select count(*) from dispatch_topology_t;

-- A good file again: recovers with no operator action.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/topo_variant_good" "$COORDINATOR_DATA_DIRECTORY/whpg_dr_topology_test";
1: select count(*) from dispatch_topology_t;

-- The dbid handshake: addresses of contents 0 and 1 are crossed
-- while the dbids stay, so each dispatch reaches a live segment
-- whose own dbid differs.  The QE refuses; the failure is permanent
-- and surfaces without burning gang-creation retries.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/topo_variant_swapped" "$COORDINATOR_DATA_DIRECTORY/whpg_dr_topology_test";
1: select count(*) from dispatch_topology_t;

-- Fail closed: an unparsable file refuses dispatch.  In a dispatch
-- session even SHOW fails, because the component table is rebuilt at
-- transaction start; the state is observable from a utility-mode
-- session, which is how a monitoring tool reads it.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/topo_variant_garbage" "$COORDINATOR_DATA_DIRECTORY/whpg_dr_topology_test";
1: select count(*) from dispatch_topology_t;
-1U: show whpg_dispatch_topology_state;

-- A missing coordinator row is also invalid.
!\retcode cp "$COORDINATOR_DATA_DIRECTORY/topo_variant_nocoord" "$COORDINATOR_DATA_DIRECTORY/whpg_dr_topology_test";
1: select count(*) from dispatch_topology_t;

-- Clearing the GUC restores stock behavior.
!\retcode gpconfig -r whpg_dispatch_topology_file --coordinatoronly --skipvalidation;
!\retcode gpstop -u;
1: select count(*) from dispatch_topology_t;
1: show whpg_dispatch_topology_state;
1: drop table dispatch_topology_t;

!\retcode rm -f "$COORDINATOR_DATA_DIRECTORY/whpg_dr_topology_test" "$COORDINATOR_DATA_DIRECTORY"/topo_variant_*;
