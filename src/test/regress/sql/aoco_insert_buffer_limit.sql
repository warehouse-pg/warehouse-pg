--
-- gp_aoco_insert_buffer_limit: a command that inserts into many ao_column
-- partitions releases the column buffers of the partitions it wrote to
-- least recently, and the optimizer sorts the rows by partition key when
-- they would otherwise keep more partitions open than fit.  The sort keeps
-- the rows of a partition in the order they are read in.
--
create schema aoco_insert_buffer_limit;
set search_path = aoco_insert_buffer_limit;

--
-- Releasing and reopening the buffers must not lose or duplicate rows.
-- The smallest limit makes every partition but the current one release
-- its buffers.  generate_series() has no statistics, so nothing here is
-- sorted for the insert.
--
set gp_aoco_insert_buffer_limit = '1kB';

create table ref (id int, k int, v text, w numeric, big text)
  distributed by (id);
insert into ref
  select i, i % 20, 'v' || i, i * 1.5,
         case when i % 50 = 0 then repeat(md5(i::text), 3000) end
  from generate_series(1, 40000) i;

create table pt (id int, k int, v text, w numeric, big text)
  using ao_column with (compresstype = zlib, compresslevel = 1)
  distributed by (id)
  partition by range (k) (start (0) end (20) every (1));
create index pt_id on pt (id);
create unique index pt_uq on pt (id, k);

-- rows of the partitions interleaved: each partition is reopened
insert into pt select * from ref where id <= 10000;
-- rows grouped by partition: each partition is released once
insert into pt select * from ref where id > 10000 and id <= 30000 order by k;
-- several inserts in one transaction, one of them rolled back
begin;
insert into pt select * from ref where id > 30000 and id <= 35000 order by k;
savepoint s;
insert into pt select id + 100000, k, v, w, big from ref;
rollback to s;
insert into pt select * from ref where id > 35000 order by k desc;
commit;

select (select count(*) from (select * from pt except select * from ref) x) as extra,
       (select count(*) from (select * from ref except select * from pt) x) as missing;

-- the block directory covers the rows written after a reopen
set enable_seqscan = off;
set optimizer = off;
select count(*), sum((p.v = r.v)::int) as matching
  from ref r join pt p on p.id = r.id and p.k = r.k
  where r.id in (7, 9999, 10001, 29999, 30001, 35001, 40000);
reset enable_seqscan;
reset optimizer;

-- a unique violation is still detected
insert into pt select * from ref where id = 5;

-- rows moved between partitions by UPDATE
update pt set k = (k + 1) % 20 where id % 3 = 0;
update ref set k = (k + 1) % 20 where id % 3 = 0;
update pt set v = v || 'x' where id % 7 = 0;
update ref set v = v || 'x' where id % 7 = 0;
delete from pt where id % 11 = 0;
delete from ref where id % 11 = 0;

-- an aborted insert leaves nothing behind and the table stays writable
begin;
insert into pt select id + 200000, k, v, w, big from ref;
rollback;
insert into pt select id + 300000, k, v, w, big from ref where id < 100;
insert into ref select id + 300000, k, v, w, big from ref where id < 100;
vacuum pt;

select (select count(*) from (select * from pt except select * from ref) x) as extra,
       (select count(*) from (select * from ref except select * from pt) x) as missing;

-- COPY goes through the same buffers
create table pt_copy (like pt) using ao_column
  distributed by (id)
  partition by range (k) (start (0) end (20) every (1));
copy (select * from ref) to '/tmp/aoco_insert_buffer_limit.data';
copy pt_copy from '/tmp/aoco_insert_buffer_limit.data';
select (select count(*) from (select * from pt_copy except select * from ref) x) as extra,
       (select count(*) from (select * from ref except select * from pt_copy) x) as missing;

-- a limit of zero keeps every partition's buffers
set gp_aoco_insert_buffer_limit = 0;
truncate pt_copy;
insert into pt_copy select * from ref;
select (select count(*) from (select * from pt_copy except select * from ref) x) as extra,
       (select count(*) from (select * from ref except select * from pt_copy) x) as missing;

--
-- Sorting for the insert.  Each partition of the target takes four
-- columns of 4 * 32kB, so a 1MB limit lets four partitions fit before
-- the rows get sorted.
--
create table tgt (id int, k int, a int, b int)
  using ao_column
  distributed by (id)
  partition by range (k) (start (0) end (20) every (1));
create table src_many as
  select i as id, i % 20 as k, i as a, i as b from generate_series(1, 20000) i
  distributed by (id);
create table src_few as
  select i as id, i % 3 as k, i as a, i as b from generate_series(1, 20000) i
  distributed by (id);
create table src_nostats as select * from src_many distributed by (id);
analyze src_many;
analyze src_few;

set gp_aoco_insert_buffer_limit = '1MB';

-- twenty partitions do not fit: sorted
explain (costs off) insert into tgt select * from src_many;
-- three do: not sorted
explain (costs off) insert into tgt select * from src_few;
-- a filter on the partition key leaves few enough partitions: not sorted
explain (costs off) insert into tgt select * from src_many where k = 5;
-- sorted by the query itself, in either direction: not sorted again
explain (costs off) insert into tgt select * from src_many order by k;
explain (costs off) insert into tgt select * from src_many order by k desc, a;
-- sorted by the query on something else: sorted by partition on top, which
-- keeps the query's order within each partition
explain (costs off) insert into tgt select * from src_many order by a;
-- nothing known about the partition key: not sorted
explain (costs off) insert into tgt select * from src_nostats;
-- only two columns are given a value, which doubles what fits; the
-- twenty partitions still do not
explain (costs off) insert into tgt (id, k) select id, k from src_many;
-- with the limit of a default configuration nothing here needs sorting
reset gp_aoco_insert_buffer_limit;
explain (costs off) insert into tgt select * from src_many;
-- no limit, no sort
set gp_aoco_insert_buffer_limit = 0;
explain (costs off) insert into tgt select * from src_many;

-- other targets are left alone: a heap partitioned table, an ao_column
-- table without partitions, and hash partitions, which have no order
set gp_aoco_insert_buffer_limit = '1kB';
create table tgt_heap (id int, k int, a int, b int)
  distributed by (id)
  partition by range (k) (start (0) end (20) every (1));
create table tgt_plain (id int, k int, a int, b int) using ao_column
  distributed by (id);
create table tgt_hash (id int, k int, a int, b int) using ao_column
  distributed by (id) partition by hash (k);
create table tgt_hash_0 partition of tgt_hash for values with (modulus 2, remainder 0);
create table tgt_hash_1 partition of tgt_hash for values with (modulus 2, remainder 1);
explain (costs off) insert into tgt_heap select * from src_many;
explain (costs off) insert into tgt_plain select * from src_many;
explain (costs off) insert into tgt_hash select * from src_many;

-- the sorted insert stores the same rows
set gp_aoco_insert_buffer_limit = '1MB';
insert into tgt select * from src_many;
insert into tgt (id, k) select id, k from src_many;
select count(*), count(distinct k) as parts, sum(a) as a, count(b) as b from tgt;

-- the rows of a partition are stored in the order they were read in: the
-- source is in order of a, and so is every partition
truncate tgt;
set gp_aoco_insert_buffer_limit = '1MB';
insert into tgt select * from src_many order by a;
select count(*) as out_of_order from (
  select a, lag(a) over (partition by gp_segment_id, k order by ctid) as prev
  from tgt) s where a < prev;

reset gp_aoco_insert_buffer_limit;
\! rm -f /tmp/aoco_insert_buffer_limit.data
drop schema aoco_insert_buffer_limit cascade;
