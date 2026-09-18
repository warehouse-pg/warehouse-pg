-- The primary removes pg_distributedlog segment files once its distributed-log
-- horizon has moved past them, and writes a truncate record.  A hot standby
-- mirror replaying that record must not drop mappings its own readers can
-- still need: their distributed snapshots come from the standby coordinator,
-- which replays its own WAL stream and may lag this mirror.  So the mirror
-- removes only what lies below its own horizon, which is advanced by the
-- snapshots dispatched to it, and drops the remaining files itself once its
-- horizon has crossed the segment.  Each of these used to be wrong: the
-- mirror truncated whatever the primary said, never moved its horizon, and
-- every standby query then failed with "could not access status of
-- transaction N"; a restart initialised the horizon to 3 and made N become 3.
--
-- The restart at the end also covers the coordinator's side of the same gap:
-- the standby coordinator learns the latest completed distributed transaction
-- id only from WAL, and a shutdown checkpoint used not to provide it.  Until
-- the next two-phase commit was replayed, its distributed snapshots treated
-- every distributed transaction as still running, and a committed table
-- showed only the rows of segments whose distributed-log horizon had already
-- moved past them.  So the first standby query after the restart runs before
-- any new commit.
--
-- One segment file holds 131072 xids (4096 entries per 32k page, 32 pages).
-- The xids are burnt on content 0 only, by one utility-session transaction
-- that assigns an xid to each of that many plpgsql subtransactions: every 64
-- of them are logged in an XACT_ASSIGNMENT record, so the mirror moves them
-- out of KnownAssignedXids as they are assigned rather than at commit, and no
-- round trip per xid is needed.  txid_current() is a 64-bit value carrying
-- the epoch; the segment file names use the 32-bit xid.  The inserts below
-- must include a row on content 0 (34 hashes there): a two-phase commit that
-- content 0 takes part in is what makes the primary wait, under remote_apply,
-- for the mirror to have replayed everything before it, the truncate record
-- included.  The test relies on hot_standby_feedback being off (query_conflict
-- resets it): with it on, the standby reader's xmin would hold the primary's
-- horizon back and no truncation would happen.

1: create table hs_dlog_t(a int) distributed by (a);
1: insert into hs_dlog_t select generate_series(1, 30);

-- the standby reader takes its snapshot now; this also parks the mirror's
-- horizon in the current segment file
-1S: begin transaction isolation level repeatable read;
-1S: select count(*) from hs_dlog_t;

-- burn content 0 past the next segment-file boundary
!\retcode port=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = 0 and role = 'p'"); cur=$(PGOPTIONS='-c gp_role=utility' psql -p $port -d postgres -Atc 'select txid_current()'); need=$(( 131072 - cur % 131072 + 300 )); PGOPTIONS='-c gp_role=utility' psql -p $port -d postgres -q -v ON_ERROR_STOP=1 -c "create temp table hs_dlog_burn(a int); do \$\$ begin for i in 1..$need loop begin insert into hs_dlog_burn values (i); exception when others then raise; end; end loop; end \$\$;" > /dev/null;

-- a distributed snapshot on the primary advances its horizon across the
-- boundary and removes the old segment file there; the two-phase insert
-- afterwards is applied on the mirror only once the truncate record has been
-- replayed
1: select count(*) from hs_dlog_t;
1: insert into hs_dlog_t values (31), (32), (34);

-- the primary has dropped the old segment file, the mirror has kept it: its
-- horizon, held back by the reader's snapshot, is still inside that segment
!\retcode port=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = 0 and role = 'p'"); seg=$(printf '%04X' $(( $(PGOPTIONS='-c gp_role=utility' psql -p $port -d postgres -Atc 'select txid_current() % 4294967296') / 131072 - 1 ))); pdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'p'"); mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); test ! -e "$pdir/pg_distributedlog/$seg" && test -e "$mdir/pg_distributedlog/$seg";

-- the reader whose snapshot predates the truncation is neither cancelled nor
-- given a wrong answer
-1S: select count(*) from hs_dlog_t;
-1S: end;
-1Sq:

-- a fresh standby snapshot sees the new rows, and moves the mirror's horizon
-- across the boundary, which drops the old segment file on the mirror too
-1S: select count(*) from hs_dlog_t;
!\retcode port=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = 0 and role = 'p'"); seg=$(printf '%04X' $(( $(PGOPTIONS='-c gp_role=utility' psql -p $port -d postgres -Atc 'select txid_current() % 4294967296') / 131072 - 1 ))); mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); test ! -e "$mdir/pg_distributedlog/$seg";
-1Sq:

-- a standby that starts after the old pages are gone answers correctly, and
-- it sees every committed row on every segment before any new commit arrives
1q:
!\retcode gpstop -ar -M fast;
-1S: select count(*) from hs_dlog_t;
-1S: select gp_segment_id, count(*) from hs_dlog_t group by 1 order by 1;
1: insert into hs_dlog_t values (33), (35), (36);
-1S: select count(*) from hs_dlog_t;
-1Sq:

1: drop table hs_dlog_t;
1q:
