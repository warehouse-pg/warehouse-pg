-- GPORCA Dynamic Seq Scan over range-partitioned tables whose leaves do
-- not all share the parent's physical column order.
--
-- GPORCA translates each leaf's partition constraint onto the root table's
-- columns when it performs static partition elimination. The constraint
-- numbers the leaf's columns by their position among the leaf's non-dropped
-- columns, so the translation must pair a root column with the same-named
-- leaf column's own position, never with the root's position. With a
-- positional mapping, a leaf whose partition key sits at a different
-- ordinal than in the root had its constraint rewritten onto whichever root
-- column sits at that ordinal: in t_part the pk=1 leaf's "pk" constraint
-- landed on the root's "k", so a predicate on the non-partition-key column
-- k pruned that leaf ("3 (out of 4)") and lost its rows. In t_mix the
-- reordered leaf's "pk" sits where the root has the text column "s", so
-- the layout guards the by-name contract across differently typed columns
-- as well.
--
-- The heap leaf and the dropped-column leaf of t_part share the root's
-- column order and only broaden the coverage. optimizer_trace_fallback
-- makes a fallback visible, so the absence of a NOTICE proves GPORCA
-- planned each query.
create schema dsq_het;
set search_path = dsq_het;
set optimizer = on;
set optimizer_trace_fallback = on;

create table t_part (id int, pk int, k int, s text)
  using ao_column distributed by (id) partition by range (pk);
create table t_part_p0 partition of t_part for values from (0) to (1);
-- reordered leaf: parent is (id, pk, k, s), this leaf is (s, k, pk, id);
-- pk sits where the root has k (same type)
create table t_part_p1 (s text, k int, pk int, id int)
  using ao_column distributed by (id);
alter table t_part attach partition t_part_p1 for values from (1) to (2);
-- heap leaf (different access method)
create table t_part_p2 (id int, pk int, k int, s text) distributed by (id);
alter table t_part attach partition t_part_p2 for values from (2) to (3);
-- leaf with a dropped column (attno gap)
create table t_part_p3 (id int, pk int, gone int, k int, s text)
  using ao_column distributed by (id);
alter table t_part_p3 drop column gone;
alter table t_part attach partition t_part_p3 for values from (3) to (4);

insert into t_part select i, i % 4, i, 'p' || i from generate_series(1, 4000) i;
analyze t_part;

-- k is NOT the partition key, so every partition must be scanned; the pk=1
-- (reordered) leaf must not be pruned.
explain (costs off) select pk, count(*) from t_part where k = 1733 group by pk;

-- predicate on k lands only in the pk=1 leaf; it must return that row.
select pk, count(*) from t_part where k = 1733 group by pk order by pk;

-- range on k that spans all four leaves: every pk must appear.
select pk, count(*) from t_part where k between 100 and 199 group by pk order by pk;

-- full row from the reordered leaf, values in the right columns.
select id, pk, k, s from t_part where id = 1733;

-- reordered leaf whose pk sits where the root has the text column s
create table t_mix (id int, pk int, s text)
  using ao_column distributed by (id) partition by range (pk);
create table t_mix_p0 partition of t_mix for values from (0) to (1);
create table t_mix_p1 (id int, s text, pk int)
  using ao_column distributed by (id);
alter table t_mix attach partition t_mix_p1 for values from (1) to (2);
insert into t_mix select i, i % 2, 'm' || i from generate_series(1, 100) i;
analyze t_mix;

-- s is not the partition key, so both leaves are scanned, and GPORCA must
-- plan it (no fallback NOTICE).
explain (costs off) select pk, count(*) from t_mix where s = 'm7' group by pk;
select pk, count(*) from t_mix where s like 'm1%' group by pk order by pk;
select id, pk, s from t_mix where id = 7;

reset optimizer_trace_fallback;
reset search_path;
reset optimizer;
-- start_ignore
drop schema dsq_het cascade;
-- end_ignore
