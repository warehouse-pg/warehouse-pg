--
-- Window top-N pre-filter: a "rank() <= N" style filter of the outer query
-- is applied on each segment before the window's Sort and Motion, by a
-- WindowTopN node or, where the node does not apply, by Sort + WindowAgg +
-- Filter (GUC whpg_enable_window_topn).  See window_topn_orca for the ORCA
-- optimizer.
--
set optimizer = off;

create schema window_topn;
set search_path = window_topn;

-- distribution key differs from the PARTITION BY column, so the final
-- WindowAgg needs a Redistribute Motion
create table wtp_t (p int, k int, v int) distributed by (v);
insert into wtp_t select i % 5, (i * 7919) % 2000, i from generate_series(1, 20000) i;
analyze wtp_t;

-- a baseline computed without the optimization, for result comparisons
set whpg_enable_window_topn = off;
create table wtp_base_rank as
  select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
  where rk <= 3;
create table wtp_base_drank as
  select * from (select p, k, v, dense_rank() over (partition by p order by k) rk from wtp_t) s
  where rk < 3;
reset whpg_enable_window_topn;

--
-- rank() <= N: a first stage below the Redistribute Motion keeps each
-- segment's candidate rows
--
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 3;

select count(*),
       (select count(*) from wtp_base_rank) as expected
from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 3;

select * from (
  (select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s where rk <= 3)
  except all
  (select * from wtp_base_rank)
) d;
select * from (
  (select * from wtp_base_rank)
  except all
  (select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s where rk <= 3)
) d;

--
-- dense_rank() < N, with ties
--
explain (costs off)
select * from (select p, k, v, dense_rank() over (partition by p order by k) rk from wtp_t) s
where rk < 3;

select * from (
  (select * from (select p, k, v, dense_rank() over (partition by p order by k) rk from wtp_t) s where rk < 3)
  except all
  (select * from wtp_base_drank)
) d;
select * from (
  (select * from wtp_base_drank)
  except all
  (select * from (select p, k, v, dense_rank() over (partition by p order by k) rk from wtp_t) s where rk < 3)
) d;

--
-- row_number() with a unique ordering, so the result is deterministic
--
explain (costs off)
select * from (select p, k, v, row_number() over (partition by p order by k, v) rn from wtp_t) s
where rn <= 2 order by p, rn;

select * from (select p, k, v, row_number() over (partition by p order by k, v) rn from wtp_t) s
where rn <= 2 order by p, rn;

--
-- operator forms: constant on the left, bigint constant, equality
--
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where 3 >= rk;

explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 3::bigint;

explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk = 1;

select p, k, count(*) from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk = 1 group by p, k order by p;

-- a PREPARE parameter is a valid bound too
prepare wtp_p(int) as
select p, count(*) from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= $1 group by p order by p;
explain (costs off) execute wtp_p(3);
execute wtp_p(3);
deallocate wtp_p;

--
-- not eligible: ">" filter, non-monotonic function, LIMIT in the subquery
--
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk > 3;

explain (costs off)
select * from (select p, k, v, percent_rank() over (partition by p order by k) pr from wtp_t) s
where pr <= 0.1;

explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t limit 100) s
where rk <= 3;

--
-- no PARTITION BY: the final WindowAgg gathers to a single segment; the
-- pre-filtered input stays sorted and is merged by the Gather Motion
--
explain (costs off)
select * from (select k, v, rank() over (order by k) rk from wtp_t) s
where rk <= 3 order by k, v;

select * from (select k, v, rank() over (order by k) rk from wtp_t) s
where rk <= 3 order by k, v;

--
-- distribution key equals the PARTITION BY column: no Motion is needed, so
-- the pre-filter is not used
--
create table wtp_d (p int, k int, v int) distributed by (p);
insert into wtp_d select * from wtp_t;
analyze wtp_d;

explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_d) s
where rk <= 3;

--
-- other window functions in the same window clause are computed only by
-- the final WindowAgg, over the complete partition
--
explain (costs off)
select * from (
  select p, k, v,
         rank() over w as rk,
         sum(v) over w as sv,
         count(*) over (partition by p) as cp
  from wtp_t
  window w as (partition by p order by k)
) s where rk <= 2 order by p, k, v;

select * from (
  select p, k, v,
         rank() over w as rk,
         sum(v) over w as sv,
         count(*) over (partition by p) as cp
  from wtp_t
  window w as (partition by p order by k)
) s where rk <= 2 order by p, k, v;

set whpg_enable_window_topn = off;
select * from (
  select p, k, v,
         rank() over w as rk,
         sum(v) over w as sv,
         count(*) over (partition by p) as cp
  from wtp_t
  window w as (partition by p order by k)
) s where rk <= 2 order by p, k, v;
reset whpg_enable_window_topn;

--
-- other functions of the same window clause that look past the current
-- row would see a smaller partition: no pre-filter for them.  Aggregates
-- whose frame ends at the current row are fine.
--
explain (costs off)
select * from (
  select p, k, v, rank() over w as rk, sum(v) over w as total
  from wtp_t window w as (partition by p order by k
                          rows between unbounded preceding and unbounded following)
) s where rk <= 2 order by p, k, v;
select * from (
  select p, k, v, rank() over w as rk, sum(v) over w as total
  from wtp_t window w as (partition by p order by k
                          rows between unbounded preceding and unbounded following)
) s where rk <= 2 order by p, k, v;

explain (costs off)
select * from (
  select p, k, v, rank() over w as rk, lead(v) over w as nxt, ntile(4) over w as nt
  from wtp_t window w as (partition by p order by k, v)
) s where rk <= 2 order by p, k, v;
select * from (
  select p, k, v, rank() over w as rk, lead(v) over w as nxt, ntile(4) over w as nt
  from wtp_t window w as (partition by p order by k, v)
) s where rk <= 2 order by p, k, v;

-- an aggregate whose frame ends at the current row sees the same rows
-- before and after the pre-filter
explain (costs off)
select * from (
  select p, k, v, rank() over w as rk, sum(v) over w as running
  from wtp_t window w as (partition by p order by k)
) s where rk <= 2 order by p, k, v;
select * from (
  select p, k, v, rank() over w as rk, sum(v) over w as running
  from wtp_t window w as (partition by p order by k)
) s where rk <= 2 order by p, k, v;

-- row_number() may cut a peer group, so a RANGE frame (the default) is not
-- safe with it, while a ROWS frame is
explain (costs off)
select * from (
  select p, k, v, row_number() over w as rn, sum(v) over w as running
  from wtp_t window w as (partition by p order by k, v)
) s where rn <= 2 order by p, k, v;
explain (costs off)
select * from (
  select p, k, v, row_number() over w as rn, sum(v) over w as running
  from wtp_t window w as (partition by p order by k, v
                          rows between unbounded preceding and current row)
) s where rn <= 2 order by p, k, v;
select * from (
  select p, k, v, row_number() over w as rn, sum(v) over w as running
  from wtp_t window w as (partition by p order by k, v
                          rows between unbounded preceding and current row)
) s where rn <= 2 order by p, k, v;

--
-- two filtered window functions of the same window clause: both are
-- computed by the first stage, the Filter has both conditions
--
explain (costs off)
select * from (
  select p, k, v,
         rank() over w as rk,
         dense_rank() over w as dr
  from wtp_t
  window w as (partition by p order by k)
) s where rk <= 3 and dr <= 2 order by p, k, v;

select * from (
  select p, k, v,
         rank() over w as rk,
         dense_rank() over w as dr
  from wtp_t
  window w as (partition by p order by k)
) s where rk <= 3 and dr <= 2 order by p, k, v;

--
-- filtered window functions of two different window clauses: the Motion
-- is added for the first window, so the input of the second one is
-- already distributed by the partitioning column and nothing is gained
--
explain (costs off)
select * from (
  select p, k, v,
         rank() over (partition by p order by k) rk,
         row_number() over (partition by p order by k, v) rn
  from wtp_t
) s where rk <= 3 and rn <= 2 order by p, k, v;

select * from (
  select p, k, v,
         rank() over (partition by p order by k) rk,
         row_number() over (partition by p order by k, v) rn
  from wtp_t
) s where rk <= 3 and rn <= 2 order by p, k, v;

--
-- two subqueries with window functions in one query: the filter of one
-- must not leak into the other
--
explain (costs off)
select a.p, count(*), min(a.k) as min_k, max(b.k) as max_k
from (select p, k, rank() over (partition by p order by k) rk from wtp_t) a,
     (select p, k, rank() over (partition by p order by k desc) rk from wtp_t) b
where a.rk <= 1 and a.p = b.p and b.rk <= 1
group by a.p order by a.p;

select a.p, count(*), min(a.k) as min_k, max(b.k) as max_k
from (select p, k, rank() over (partition by p order by k) rk from wtp_t) a,
     (select p, k, rank() over (partition by p order by k desc) rk from wtp_t) b
where a.rk <= 1 and a.p = b.p and b.rk <= 1
group by a.p order by a.p;

--
-- a shared CTE below the window: its Share Input Scan runs in the slice of
-- the first stage
--
set gp_cte_sharing = on;
explain (costs off)
with c as (select p, k, v from wtp_t)
select p, k, v, rk from (select p, k, v, rank() over (partition by p order by k) rk from c) s
where rk <= 2
union all
select -1, count(*)::int, null, null from c
order by 1, 2, 3;
with c as (select p, k, v from wtp_t)
select p, k, v, rk from (select p, k, v, rank() over (partition by p order by k) rk from c) s
where rk <= 2
union all
select -1, count(*)::int, null, null from c
order by 1, 2, 3;
reset gp_cte_sharing;

--
-- WindowTopN keeps the top rows of each partition with a hash table and
-- heaps instead of Sort + WindowAgg + Filter.  The node holds the kept rows
-- in memory, so when they are not expected to fit, the Sort form is used
-- instead (here on an input that an index already delivers in window
-- order); results are the same either way.
--
create index wtp_t_pk on wtp_t (p, k);
set work_mem = '64kB';
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 200;
select count(*) from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 200;
set whpg_enable_window_topn = off;
select count(*) from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 200;
reset whpg_enable_window_topn;
-- a bound that fits keeps the node
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 3;
reset work_mem;
drop index wtp_t_pk;

-- the node hashes its input on the PARTITION BY columns, so a partition key
-- of a type without a hash opclass gets the Sort form as well
explain (costs off)
select * from (select p, k, v, rank() over (partition by row(p) order by k) rk from wtp_t) s
where rk <= 3;
select count(*),
       (select count(*) from wtp_base_rank) as expected
from (select p, k, v, rank() over (partition by row(p) order by k) rk from wtp_t) s
where rk <= 3;

-- the bound as a parameter is evaluated once per execution
prepare wtp_topn(int) as
select p, count(*) from (select p, k, v, dense_rank() over (partition by p order by k desc) dr from wtp_t) s
where dr <= $1 group by p order by p;
execute wtp_topn(2);
execute wtp_topn(0);
deallocate wtp_topn;

-- a NULL bound keeps nothing
select count(*) from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= null::int;

-- the node is rescanned when a parameter of its input changes
select t.p,
       (select count(*)
        from (select k, rank() over (order by k) rk from wtp_t w where w.p = t.p) s
        where rk <= 3) as top3
from (values (0), (1), (2)) t(p) order by 1;

-- NULLS FIRST / DESC ordering is honoured by the heaps
create table wtp_n (p int, k int, v int) distributed by (v);
insert into wtp_n select i % 3, case when i % 7 = 0 then null else i % 50 end, i from generate_series(1, 3000) i;
analyze wtp_n;
set whpg_enable_window_topn = off;
create table wtp_base_n as
  select * from (select p, k, v, rank() over (partition by p order by k desc nulls first) rk from wtp_n) s
  where rk <= 4;
reset whpg_enable_window_topn;
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k desc nulls first) rk from wtp_n) s
where rk <= 4;
select count(*), (select count(*) from wtp_base_n) as expected from (
  (select * from (select p, k, v, rank() over (partition by p order by k desc nulls first) rk from wtp_n) s where rk <= 4)
  intersect all
  (select * from wtp_base_n)
) d;

-- when the kept rows do not fit the operator's memory, the node returns
-- everything it has and passes the rest of its input through; the result
-- is unchanged
set statement_mem = '1000kB';
select count(*), min(k), max(k) from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 100000;
set whpg_enable_window_topn = off;
select count(*), min(k), max(k) from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 100000;
reset whpg_enable_window_topn;
reset statement_mem;

--
-- GUC off: a single WindowAgg
--
set whpg_enable_window_topn = off;
explain (costs off)
select * from (select p, k, v, rank() over (partition by p order by k) rk from wtp_t) s
where rk <= 3;
reset whpg_enable_window_topn;

drop schema window_topn cascade;
