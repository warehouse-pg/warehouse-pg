--
-- Window top-N pre-filter with the ORCA optimizer: a "rank() <= N" style
-- filter of the outer query becomes a WindowTopN node below the window's
-- Motion (see window_topn for the planner).
--
set optimizer = on;
set optimizer_trace_fallback = on;

create schema window_topn_orca;
set search_path = window_topn_orca;

create table wto_t (p int, k int, v int) distributed by (v);
insert into wto_t select i % 5, (i * 7919) % 2000, i from generate_series(1, 20000) i;
analyze wto_t;

-- baselines without the optimization
set whpg_enable_window_topn = off;
create table wto_base_rank as
  select * from (select p, k, v, rank() over (partition by p order by k) rk from wto_t) s
  where rk <= 3;
create table wto_base_drank as
  select * from (select p, k, v, dense_rank() over (partition by p order by k desc) dr from wto_t) s
  where dr < 3;
reset whpg_enable_window_topn;

--
-- rank() <= N
--
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wto_t) s
where rk <= 3;

select count(*), (select count(*) from wto_base_rank) as expected from (
  (select * from (select p, k, v, rank() over (partition by p order by k) rk from wto_t) s where rk <= 3)
  intersect all
  (select * from wto_base_rank)
) d;

--
-- dense_rank() < N, constant on the left, equality
--
explain (costs off)
select * from (select p, k, v, dense_rank() over (partition by p order by k desc) dr from wto_t) s
where 3 > dr;

select count(*), (select count(*) from wto_base_drank) as expected from (
  (select * from (select p, k, v, dense_rank() over (partition by p order by k desc) dr from wto_t) s where 3 > dr)
  intersect all
  (select * from wto_base_drank)
) d;

explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wto_t) s
where rk = 1;

select p, k, count(*) from (select p, k, v, rank() over (partition by p order by k) rk from wto_t) s
where rk = 1 group by p, k order by p;

--
-- row_number() with a unique ordering
--
explain (costs off)
select * from (select p, k, v, row_number() over (partition by p order by k, v) rn from wto_t) s
where rn <= 2 order by p, rn;

select * from (select p, k, v, row_number() over (partition by p order by k, v) rn from wto_t) s
where rn <= 2 order by p, rn;

--
-- no PARTITION BY: the final window gathers to one segment
--
explain (costs off)
select * from (select k, v, rank() over (order by k) rk from wto_t) s
where rk <= 3 order by k, v;

select * from (select k, v, rank() over (order by k) rk from wto_t) s
where rk <= 3 order by k, v;

--
-- not eligible: ">" filter, a function of the same window that looks past
-- the current row
--
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wto_t) s
where rk > 3;

explain (costs off)
select * from (
  select p, k, v, rank() over w as rk, lead(v) over w as nxt
  from wto_t window w as (partition by p order by k)
) s where rk <= 2;

-- an aggregate of the same window is fine while its frame ends at the
-- current row (in ROWS mode for row_number(), which may cut a peer group)
explain (costs off)
select * from (
  select p, k, v, rank() over w as rk, sum(v) over w as running
  from wto_t window w as (partition by p order by k)
) s where rk <= 2 order by p, k, v;
select * from (
  select p, k, v, rank() over w as rk, sum(v) over w as running
  from wto_t window w as (partition by p order by k)
) s where rk <= 2 order by p, k, v;
explain (costs off)
select * from (
  select p, k, v, rank() over w as rk, sum(v) over w as total
  from wto_t window w as (partition by p order by k
                          rows between unbounded preceding and unbounded following)
) s where rk <= 2;
explain (costs off)
select * from (
  select p, k, v, row_number() over w as rn, sum(v) over w as running
  from wto_t window w as (partition by p order by k, v)
) s where rn <= 2;
explain (costs off)
select * from (
  select p, k, v, row_number() over w as rn, sum(v) over w as running
  from wto_t window w as (partition by p order by k, v
                          rows between unbounded preceding and current row)
) s where rn <= 2 order by p, k, v;
select * from (
  select p, k, v, row_number() over w as rn, sum(v) over w as running
  from wto_t window w as (partition by p order by k, v
                          rows between unbounded preceding and current row)
) s where rn <= 2 order by p, k, v;

--
-- distribution key equals the PARTITION BY column: no Motion is needed, but
-- WindowTopN still spares the Sort of the final window the rows it drops
--
create table wto_d (p int, k int, v int) distributed by (p);
insert into wto_d select * from wto_t;
analyze wto_d;
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wto_d) s
where rk <= 3;

select count(*), (select count(*) from wto_base_rank) as expected from (
  (select * from (select p, k, v, rank() over (partition by p order by k) rk from wto_d) s where rk <= 3)
  intersect all
  (select * from wto_base_rank)
) d;

-- the same when the input is already on a single segment (the LIMIT
-- gathers it there)
explain (costs off)
select * from (
  select p, k, v, rank() over (partition by p order by k) rk
  from (select p, k, v from wto_t limit 100000) i
) s where rk <= 3;

select count(*), (select count(*) from wto_base_rank) as expected from (
  (select * from (
     select p, k, v, rank() over (partition by p order by k) rk
     from (select p, k, v from wto_t limit 100000) i
   ) s where rk <= 3)
  intersect all
  (select * from wto_base_rank)
) d;

--
-- the input of the first stage may hold Share Input Scans, as a ROLLUP does
-- with this optimizer: they run in the slice of the WindowTopN
--
explain (costs off)
select * from (
  select p, k, sum(v) as s, rank() over (partition by p order by sum(v) desc) rk
  from wto_t group by rollup (p, k)
) s where rk <= 2 order by p, k;
select * from (
  select p, k, sum(v) as s, rank() over (partition by p order by sum(v) desc) rk
  from wto_t group by rollup (p, k)
) s where rk <= 2 order by p, k;

--
-- GUC off
--
set whpg_enable_window_topn = off;
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wto_t) s
where rk <= 3;
reset whpg_enable_window_topn;

drop schema window_topn_orca cascade;
