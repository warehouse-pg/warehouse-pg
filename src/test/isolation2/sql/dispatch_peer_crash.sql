-- The QD must not wait forever when a segment goes through a crash reset
-- while a statement with several slices is running there.
--
-- The QEs of the lower slices have finished and sit idle when the reset
-- kills every backend of the segment.  Their sockets are readable from then
-- on, and they used to stay in the set the dispatcher waits on, so the wait
-- never timed out, the cancel request was never sent, and the QEs on the
-- other segments ran on while the QD spun.

-- start_matchsubs
-- m/seg0 [0-9.]+:\d+ pid=\d+/
-- s/seg0 [0-9.]+:\d+ pid=\d+/seg0 IP:PORT pid=PID/
-- end_matchsubs

create extension if not exists gp_inject_fault;

create table dispatch_peer_crash_t (a int, b int) distributed by (a);
insert into dispatch_peer_crash_t select i, i % 10 from generate_series(1, 1000) i;
analyze dispatch_peer_crash_t;
create table dispatch_peer_crash_sink (a int) distributed by (a);

-- true once this segment has idle QEs of the statement below: its lower
-- slices are done here
create or replace function dispatch_peer_crash_wait() returns bool as $$
declare n int; /* in func */
begin /* in func */
    for i in 1 .. 600 loop /* in func */
        select count(*) into n from pg_stat_activity /* in func */
        where state = 'idle' and query like 'insert into dispatch_peer_crash_sink%'; /* in func */
        if n >= 2 then return true; end if; /* in func */
        perform pg_sleep(0.1); /* in func */
    end loop; /* in func */
    return false; /* in func */
end $$ language plpgsql;

-- Two scans feed a join that sleeps on every row: the scans finish at once,
-- the join slice runs for minutes, and the QD waits in the dispatcher for
-- the whole statement, as it does for any statement that returns no rows.
1: set optimizer = off;
1&: insert into dispatch_peer_crash_sink select x.a from dispatch_peer_crash_t x join dispatch_peer_crash_t y on x.b = y.b where pg_sleep(0.01) is not null and x.a + y.a > 0;

0U: select dispatch_peer_crash_wait();
0Uq:

-- crash seg0 from another session
2: select gp_inject_fault('start_prepare', 'panic', dbid) from gp_segment_configuration where role = 'p' and content = 0;
2: create table dispatch_peer_crash_boom (a int);

-- the statement fails; without the fix it stays here for minutes
1<:
1q:
2q:

-- wait for seg0 to come back, with a gang creation that retries for long enough
3: set gp_gang_creation_retry_timer = 10000;
3: select count(*) from dispatch_peer_crash_t;
3: select count(*) from dispatch_peer_crash_sink;
3: select gp_inject_fault('start_prepare', 'reset', dbid) from gp_segment_configuration where role = 'p' and content = 0;
3: select wait_until_all_segments_synchronized();
3: select content, role, preferred_role, mode, status from gp_segment_configuration where content = 0 order by role;

3: drop table dispatch_peer_crash_t, dispatch_peer_crash_sink;
3: drop function dispatch_peer_crash_wait();
3q:
