# Runtime filters for hash joins

A hash join knows, once its hash table is built, exactly which join keys the
inner side holds. A runtime filter hands that knowledge to the probe side's
scan, so that rows whose key cannot be in the table are dropped where they are
read, before they are assembled, projected, or handed up to the join. The
idea is as old as Bloomjoin (Mackert & Lohman, 1986) and is in every MPP
engine under some name -- bitmap filter, join filter, dynamic filter,
sideways information passing; this is WarehousePG's implementation of the
*local* form: join and probe scan in the same slice, no network in between.

Code:

| piece | file |
|---|---|
| the filter: a split-block Bloom filter in a bytea, build/insert/finish/probe | `src/backend/utils/adt/runtimefilter.c` |
| the probe: `RuntimeFilterExpr` (primnodes.h), the clause the planner puts in the scan's qual, which the executor compiles to a step of its own (`EEOP_RUNTIME_FILTER`); no catalog entry | same file; `execExpr.c`, `execExprInterp.c` |
| the planner pass: which joins, which scan, the Param, the clause | `src/backend/optimizer/plan/planrtfilter.c` |
| hooks: after `set_plan_references()` for the Postgres planner (the plan in its final form, as GPORCA's comes), after the Staged Scan pass for GPORCA | `planner.c` (`standard_planner`), `orca.c` (`optimize_query`) |
| building the filter while the table is built | `nodeHash.c` (`MultiExecPrivateHash`) |
| publishing it through the Param; the table built before the first probe row when the filter leads a Staged Scan | `nodeHashjoin.c` (`ExecHashJoin*RuntimeFilter`) |
| the Staged Scan reading the Param inside the access method | `nodeSeqscan.c`, `aocsam.c`, `aocs_stagedscan.c`, `stagedscancost.c` |
| plan fields | `HashJoin.rf_paramids` in `plannodes.h`; the clause itself is in `SeqScan.scan.plan.qual` |
| EXPLAIN | `show_runtime_filters()` in `explain.c` |
| GUCs | `guc_gp.c`, listed in `sync_guc_name.h` |
| test | `src/test/regress/sql/runtime_filter.sql` |

## How it runs

**Planning.** After either optimizer has finished its plan, a pass walks it
(`runtimefilter_decide_plan()`). For every hash join it looks for the scan the
probe rows come from: the join's outer input, through Result nodes and down
the outer side of inner, left, semi and anti joins -- a star join is a chain
of hash joins over one fact scan, and every dimension's filter can be tested
there -- stopping at a Motion, a Material, a Sort, a right or full join, an
Append or anything else. If that is a SeqScan or DynamicSeqScan of a base
relation and the join's outer hash key is a column of it, carried down the
outer side unchanged, the pass

- allocates an exec Param (`paramExecTypes`, type internal),
- puts a `RuntimeFilterExpr` over that Param, the column, the join's hash
  operator and the survivor estimate after the scan's own clauses -- they
  are cheaper and already in cost order, so the filter sees only the rows
  they pass,
- records the Param on the join (`rf_paramids`),
- and, for an AOCO table, decides the Staged Scan again with the new clause
  in the predicate (`stagedscan_decide_plan_scan()`), so that the join key
  can lead Phase A. A clause that would cost a staged scan its Staged Scan
  is left out again: the Staged Scan model re-costs one filter column as if
  it were clustered (a single-stage Phase A can fall back when the
  survivors turn out dense) but takes a multi-column predicate at its
  pessimistic estimate, and the staged read of the first key is worth more
  than a second clause in the ordinary path. The joins are visited
  bottom-up, so the lowest join's filter is the one a scan keeps.

The clause's selectivity -- the join's estimated output rows over the probe's
rows, exact when a probe row matches at most one inner row and an
overestimate of the survivors otherwise -- travels in the clause's fourth
argument and is returned by the function's planner support, so
`clause_selectivity()` and the Staged Scan cost model treat the clause like
any other. The hash function is the operator's left-hand one, the same
`ExecHashTableCreate()` applies to the probe side.

**One key of several.** A join on several keys (TPC-DS joins sales to
returns on `(ticket_number, item_sk)`) gets a filter over one of them. A
probe row passes a filter over one key when that key alone has a match,
whatever the others do, so the pass picks the key expected to let the fewest
rows through, estimated per key from the statistics
(`rf_key_match_fraction()`): the inner side's distinct values of the key --
its rows, capped by the column's distinct values when the key is a column
of some relation (`rf_trace_column()` follows it down the inner plan) --
over the probe column's distinct values, the inner side's keys being taken
for a subset of the probe's, as a foreign key's are. For the returns join
`item_sk` covers every item and `ticket_number` a few thousand of millions,
so the filter is over `ticket_number`. Rows in a plan are a segment's; a
segment's inner rows cover the same share of the probe column whether they
are the whole inner side (a Broadcast) or its hash range, except when that
range is the probe table's own distribution on the key -- then the segment's
probe rows are that range too, and the inner side's rows over all segments
count. The share is never put below the join's own estimate, and the usual
selectivity gate applies to it; a join whose probe column has no statistics
gets no filter. The Hash node hashes the chosen key by itself into the
filter (`HashJoin.rf_keyidx`; the hash value it computes for the table
combines all keys), one more evaluation of that key per inner row, and the
clause hashes the probe column the same way.

**Executing.** The join initializes its Param to NULL before its children are
initialized (`ExecInitHashJoin`). When it builds the hash table it hands the
Hash node a builder sized from the planner's estimate of the inner rows; the
Hash node inserts every tuple's hash value -- the one it computes anyway for
the bucket -- and the join then points the Param at the finished filter. A
join whose probe is a Staged Scan led by the filter builds the table before
it asks for a probe row: the scan's first batch of survivors is the sample
that decides whether the staged read pays, and a sample taken without the
filter would have it fall back. Any other join keeps the executor's shortcut
of fetching one outer row first, to end without a hash table when the outer
is empty; that row went untested by the filter and is probed in the table
like any row the filter lets through. On a rebuild (a rescan with the inner's
parameters changed) the Param goes back to NULL and a new filter takes its
place; a rescan that keeps the table keeps the filter, and the probe scan is
read again through it.

The probe side's scan evaluates the clause per row like any qual. On a heap
or AO row table that is `ExecQual()` after the row is fetched: it saves the
hash table probe, with its cache misses, for every row the filter rejects.
On an AOCO table the clause is part of the Staged Scan's predicate, so Phase
A reads the join key column alone and the rows it drops never have their
other columns decompressed; that is where most of the time goes. The AOCO
access method evaluates its stage quals in standalone ExprContexts, which
carry no Param values; the scan node hands it the executor's
(`AOCSScanDescData.stagedParamExecVals`), and the applicability checks that
decline quals with Params make an exception for this clause.

When the key column is of a fixed-width integer-like type (int2/int4/int8,
date, timestamp, timestamptz) the clause also joins the Staged Scan's raw-datum
fast path (`stagedscan_compile_simple()`): a NULL-free block in the original
datum layout is tested value by value off its datum array, each value hashed
-- inline for the integer hash functions, through the operator's own hash
support function otherwise -- and looked up in the filter, with no tuple
formed and no `ExecQual()`. The filter is bound from the Param once per block
(`stagedscan_rf_bind()`), and the watch on the filter's usefulness below
counts those rows too. A
block the fast path cannot take (NULLs, RLE/delta layout) goes row by row
through the qual's probe as before.

The probe returns true for a NULL filter (not built yet, a Parallel Hash)
or a dead one (see below) and false for a NULL key: a strict hash operator
never matches a NULL, and the join types that get a filter drop such rows.
A join over a non-strict hash operator, which hashes NULL as 0 and may match
it (`ExecHashGetHashValue()`), gets no filter.
It also watches the filter's usefulness, 65536 rows at a time: a filter that
let more than `whpg_runtime_filter_disable_ratio` of them through is rested,
and the rows go straight to the join -- the estimate was wrong, or the keys
are all there, and every further probe would be a hash and a cache line for
nothing. The rest is not for good, because what a filter removes depends on
where the scan is: a fact table loaded in key or time order has the rows
that match a dimension together, and a filter that passes every row of the
recent partition may remove every row of the older ones. So after 65536
rows have gone by, one window of 4096 is probed as a sample. If the sample
shows the filter removing rows it is back at work, judged 65536 rows at a
time as before; if not it rests twice as long, up to 262144 rows. A filter
that never helps is thus probed for one row in 65 at most, and one that
starts to help is back within 262144 rows. The tally is kept in the filter
itself (`RuntimeFilter.seen`, `.passed`, `.rest_rows`), so every probe of
one filter in a process -- the qual's, the Staged Scan fast path's, each
partition's scan under a dynamic scan -- counts into the same tally and
rests it together, rather than each starting over; a rebuilt hash table
brings a new filter, judged afresh.
A filter at rest costs the rows next to nothing. The clause compiles to a
test first (`EEOP_RUNTIME_FILTER_LIVE`) that jumps over the key's evaluation
and the probe when there is no filter to probe, and counts the row off the
filter's rest. The scan, which noted the filter clauses of its qual at init,
asks every 4096 rows whether any filter it reads is being probed; while
none is, it runs its qual without them -- none at all when nothing else was
there (`SeqNext`) -- and counts the rows off the rests itself, a window at
a time, until the first filter wakes and the full qual is back. A qual that
holds a SubPlan is the exception: it is compiled once and kept whole, since a
second compilation would give the SubPlan a second state on the scan, whose
hash table is built again after a rescan of the subplan, and a subplan with
a Motion in it cannot be rescanned. Such a scan pays the clause's first test
for each row while its filter rests, next to a subplan probe. The Staged
Scan's fast path counts them a block at a time. A join with no filter to
publish -- none built, dropped as too full, or `whpg_enable_runtime_filter`
switched off since the plan was made -- publishes a dead one
(`runtimefilter_dead()`), which is never probed and never wakes, so that the
scan can tell it from a filter yet to come and drop the clause for good; a
rescan puts the full qual back.
The clause is a node of its own, `RuntimeFilterExpr` (primnodes.h), which
only the planner writes: SQL has no syntax for it, and it rests on no
catalog entry -- no `pg_proc` row, no catalog version change -- so a minor
release can carry it onto an existing cluster. For the same reason its tag
is the last in `NodeTag` (an earlier position would renumber every tag after
it) and the executor state it needs sits at the end of `SeqScanState`,
`HashJoinState` and `HashState`, never in `ScanState` or `PlanState`, which
every other node embeds: an extension built against an earlier 7.x release
sees the node tags and struct layouts it was compiled with. The executor
compiles it to
one step (`EEOP_RUNTIME_FILTER`, `execExpr.c`) that evaluates the key and
probes the filter without going through fmgr; the JIT calls the same C
function (`ExecEvalRuntimeFilter()`). The step's setup
(`runtimefilter_probe_init()`) checks, once per plan node, that the hash
operator takes the key's type and hashes it (an array key needs a hash
function for its elements, `op_hashjoinable()`), and the probe checks that
the Param holds a filter, as guards against a planner or executor fault --
and a clause that fails a check lets every row through from then on, with a
LOG line, rather than fail the query: a filter only ever drops rows the join
would drop, so an untrusted one is no filter, not an error. The planner pass
makes the same tests before it writes a clause, so the guards should never
fire.

**The filter** is a split-block Bloom filter: 256-bit blocks of eight 32-bit
words, one bit per word, so a probe touches a single cache line; it is fed
the hash table's own 32-bit hash values, spread to 64 bits by a mixing
function, block from the top half, bits from the bottom. It is sized at 16
bits per expected key (about 1% false positives; the rate cannot fall below
the number of keys over 2^32, since the filter is built from 32-bit hash
values: 0.01% at 500,000 keys in a segment's filter, 0.1% near 5 million),
never below 8 kB and never above `whpg_runtime_filter_max_size`. The rows
that arrive then decide what becomes of it. Should it come out too full --
more than 80% of its bits set once the table is built, about 17% false
positives and climbing fast beyond -- it is built again from the hash
table at the size that many rows call for, when every inner tuple is still
in memory (a single batch) and that size is within the limit: one pass over
the table, far cheaper than the probes it saves; otherwise it is dropped
rather than probed for nothing. Fullness is judged on the bits set, not on
the tuples inserted, so an inner side of many rows over few keys keeps its
filter however low the row estimate it was sized from. Should it come out
nearly empty -- the estimate far above the rows -- it is folded down to the
size its keys need, each halving ORing the upper half of the blocks into
the lower (a key's block is its hash masked to the block count, so every
key stays in), so that the probes stay in cache. Whether the filter is
worth probing is left to the probe's own tally (the rest): how many
keys it holds against how many rows the probe has says nothing of what it
would remove, since the keys may mostly be absent from the probe. A false
positive costs the hash table probe the join would have made anyway.

**Integer keys get an exact filter when they can.** Over a key hashed as an
integer (int2, int4, int8, date, timestamp, timestamptz) the join reads the
keys' range off the table once it is built, and when a bitmap over the
range is no larger than the Bloom filter for that many rows would be, the
bitmap takes its place (`RF_FLAG_BITMAP`): one bit per value, no false
positives, and a probe is a subtraction and a bit test instead of a hash.
TPC-DS's surrogate keys are dense, so a dimension's hundred dates, or the
thirty thousand customers of a state, are bitmaps of a few kilobytes;
keys spread over billions keep the Bloom filter, tagged with their range
(`RF_FLAG_INTRANGE`) so that a key outside it is rejected without a hash.
The Staged Scan's fast path, which has the raw value in hand, tests the
bitmap directly; the qual's probe reads the key as the integer its hash
function would have hashed. Only a join whose table is all in memory (one
batch) can read the range; a spilled join keeps the Bloom filter as built.

## What is guaranteed

The join's answer is unchanged. The filter can only reject a probe row whose
key is not in the hash table (Bloom filters have no false negatives, and the
probe uses the same hash function and value as the table), and for the join
types that get a filter such a row never contributes to the output: INNER,
SEMI, RIGHT (the hash side is the preserved one), and the dedup-semi
variants. LEFT, FULL, ANTI and NOT-IN joins keep the rows the filter would
drop, and get none. An IS NOT DISTINCT FROM join (`hashqualclauses`) matches
NULLs, and gets none. A row with a NULL key is rejected; a strict hash
operator never matches it, and a join over a non-strict one gets none. The
Staged Scan's rules carry over: the clause cannot fail -- its filter is a
Param the join sets to NULL or to a filter it built, its operator and key
type paired by the planner -- so it may be decided at any stage, ahead of
clauses that can fail; `stagedscan_clause_can_fail()` knows it by its node
type, which only the planner writes (`runtimefilter_is_clause()`).

Passing a lower join on the way to the scan is safe for the same reason. A
probe row the filter drops before an inner, left, semi or anti join (the
fact on the outer side) takes only its own descendants with it: every row the
join derives from it carries the key column unchanged, so the upper join
would have dropped them all. A RIGHT or FULL join is not passed: an inner
row matched by nothing else would then be emitted null-extended in the
dropped row's place, a row the plan without the filter never produced.
Neither are the dedup-semi variants, which choose between duplicates by a
row identity a dropped row could alter. A key taken from a lower join's
inner side stops the search.

Nothing about the plan's shape changes: the same nodes, the same Motions, one
more clause on one scan per filter. A plan without a filter runs exactly as
before; a plan with one whose filter is NULL runs as before plus one cheap
call per row.

## What is declined

- a probe under a Motion (the filter would have to cross slices; see below),
  under an Append, a right or full join, a dedup-semi join, a Sort or
  anything but a Result or an inner, left, semi or anti join whose outer side
  carries the key;
- a probe under a Material: a Material rescanned with its child unchanged
  replays what it stored, so rows the previous filter dropped would never be
  read again after the hash table is rebuilt with other keys (the inner side
  parameterized by an outer loop); the rows a filter drops must come from a
  scan that is re-read on every rescan;
- a join key that is not a plain column of the scan (`k + 1`, a function);
- a join on several keys when no key is such a column under a hashable,
  strict operator, when the probe's key columns have no statistics to choose
  one by, or when no key alone is expected to drop enough probe rows (see
  "One key of several" below);
- parallel-aware joins or scans (the Param would have to reach the workers);
- plans under row-level security, as for the Staged Scan;
- a probe estimated below `whpg_runtime_filter_min_probe_rows`, a join
  expected to keep more than `whpg_runtime_filter_max_selectivity` of the
  probe rows, or an inner side that would need a filter above
  `whpg_runtime_filter_max_size` to stay useful. The first two, and the
  statistics a join on several keys needs to choose one by, hold under
  `whpg_runtime_filter_mode = estimate`; `eager` plans the filter anyway
  and `auto` does so when the probe's key column has no statistics, leaving
  the rows that arrive to decide (see "The filter"). Rows are a segment's. In
  a slice GPORCA runs on one segment (a join of replicated tables) its
  estimate counts a replicated table read there once per segment of the
  cluster; the pass divides that back by the segment count before the
  gates, so that a join of two replicated tables is judged on what the one
  segment reads.

## GUCs

| GUC | default | meaning |
|---|---|---|
| `whpg_enable_runtime_filter` | on | master switch, both optimizers. The executor reads it too: a plan cached while it was on builds no filter once it is off, and EXPLAIN shows none |
| `whpg_runtime_filter_mode` | estimate | how far the planner pass trusts its estimates: `estimate` gates a filter on them (the three developer GUCs below); `eager` plans one for every join whose shape allows it and leaves the rows that arrive to size it, publish it or not, and switch it off; `auto` is `eager` for a probe whose key column has no statistics (a table never analyzed), `estimate` otherwise. The size gate stays in every mode |
| `whpg_runtime_filter_max_size` | 4 MB | the largest filter planned (64 kB to 64 MB); a denser one is dropped at run time. The filter is memory of its own: up to this much per filtered join and segment process on top of the join's share of `statement_mem`, which the hash table does not shrink to make room for |
| `whpg_runtime_filter_max_selectivity` | 0.5 | developer: no filter when more of the probe is expected to match |
| `whpg_runtime_filter_min_probe_rows` | 10000 | developer: no filter for a smaller probe (rows as the plan node estimates them) |
| `whpg_runtime_filter_disable_ratio` | 0.9 | developer: a filter letting more of the probe rows through than this is rested by the scan, and sampled again later; 1 never rests a filter |
| `whpg_runtime_filter_explain` | off | developer: EXPLAIN adds `Runtime Filter: declined: <reason>` under every hash join the planner looked at and gave no filter (join type, probe under a Motion, probe not a scan, too small, too many matches, no key of several selective enough, ...) |

The first five are synchronized to the segments; the last is read by EXPLAIN
on the coordinator.

EXPLAIN shows the clause apart from the predicate, by the column it filters,
and EXPLAIN ANALYZE counts what the filters removed:

```
->  Staged Seq Scan on rf_fact_aoco f (actual rows=1142 loops=1)
      Runtime Filter: k
      Rows Removed by Runtime Filter: 196663
      Stages: k
      Stage Survivors: 3337 of 200000 rows
```

The count is summed over the segments, like the Staged Scan's counts below
it (196663 removed and 3337 survivors of the 200000 rows), wherever the
clause ran: the scan's qual, Phase A's fast path, a stage, or the ordinary
scan after a fallback. `Rows Removed by Filter` keeps its usual meaning, the
rows the scan's own predicate removed on the one segment whose figures
`actual rows` and that line report; the filters' rows are left out of it,
and a scan with no predicate of its own has no such line. On a table with
deleted rows the
filter's count may include deleted rows it rejected before their visibility
was checked.

## Measured

Development cluster, 3 primaries on one machine, optimized build, warm
cache, GPORCA. A 6M-row fact of 21 columns (`bf_fact`: 19 integers, one md5
text, AOCO zstd) joined to a 100k-key dimension of which 1% passes the
filter, so 1% of the fact rows have a match; the join keys are spread
uniformly over the fact. Four columns aggregated. Medians of seven
interleaved warm runs, two passes.

| probe table | filter off | filter on | plan with the filter |
|---|---|---|---|
| heap copy of the fact | 248-273 ms | 117-134 ms | `Seq Scan ... Runtime Filter: k` |
| AOCO zstd | 208-217 ms | 166-176 ms | `Staged Seq Scan ... Runtime Filter: k`, every segment file fell back |

On the heap table the saving is the hash table probe (hash, bucket, compare)
for 99% of the rows. On the AOCO table the planner made the scan a Staged Scan
with the filter leading Phase A, but the first-batch density check handed
every segment file back to the ordinary scan: with the keys spread uniformly,
every 8000-row varblock of the key column holds some 80 matches, so Phase B
could skip no block and, since an AOCO varblock is read forward datum by
datum, would advance through nearly every datum of the other columns anyway
(`stagedscan_survivors_too_dense()`). The filter then ran as the first clause
of the ordinary scan, and the gain is again the probe. The same table with a
*clustered* 1% predicate keeps the Staged Scan (`Stage Survivors: 60000 of
6000000 rows`), and so would a filter whose matches cluster in insertion
order -- a dimension filtered to recent dates against a fact loaded by date,
say -- or are sparse enough to leave most blocks empty.

**A useless filter**, a 6M-row heap fact against a 100k-key dimension every
key of which it holds (every probe row has a match), with the selectivity
gate lifted so that a filter is planned anyway: 235 ms with no filter;
269 ms with the filter never rested (`whpg_runtime_filter_disable_ratio =
1`); 243 ms at the default -- the scan rests the filter after its first
65536 rows, and what remains is those rows and the samples. (Without the
rest, a wrong estimate would cost a good part of what a right one gains.)

**A filter that is useless only at first**, a 6.3M-row AO row fact whose
first 300,000 rows all have a match in a 1000-key dimension and whose other
6M have none, the shape of a table loaded in key or time order: 129 ms with
no filter, 86 ms with it. The filter passes every one of the first 65536
rows a segment and is rested; the second sample, 131072 rows on, finds no
match and puts it back to work, and it removes 5.9M of the 6M rows that
have none (`Rows Removed by Runtime Filter: 5906784`), within a millisecond
of a filter that never rests.

**The threshold of the rest.** The ratio was 0.95 until it was measured.
Filters forced at known pass rates on SF30's `web_sales` (21.6M rows; item
subsets chosen in md5 order so that 80.00, 88.00, 93.00, 97.00 and 100% of
the rows keep a match), against a 1.2 GB heap copy and the AOCO table, a
`count(*)` and one `sum` over the join, five interleaved rounds, medians. A
filter that is kept costs the heap probe +4% at 80% pass, +9% at 88%, +11%
at 93%, +14% at 97% and +15% at 100%, and the AOCO probe +2% to +10% (an
ordinary scan: the Staged Scan model declines a stage that keeps that
much); once rested the time is close to the filter-off time, so the ratio
decides only where the loss stops. At 93% pass a ratio of 0.95 keeps the
filter (heap 964 ms against 841 off) and 0.9 gives it up (848 ms); at 88%,
0.9 keeps it (923 ms against 829) and 0.8 gives it up (832 ms). The build
side here is 40,000 keys, a hash table that stays in cache, so a saved hash
probe is worth about four filter probes and the break-even is near 75%; a
larger build side, a Staged Scan or more work downstream of the join move it
up. Across SF30 under `eager`, the filters the gates would have declined
sort by their real pass rate: those passing 90-95% (q31, q51, q72) only
cost, +2% to +7%; those passing 80-90% both win (q15 -27% on filters
passing 81% and 87%, whose dropped rows are spared every later join and
Motion) and lose (q10 +23%); those passing 95% and more are rested and sit
in the noise. So the default is 0.9, and not lower: between 80% and 90% a
filter is as likely to pay as to cost, and above 90% it only costs.

**TPC-DS, scale factor 10**, the same cluster, the repository's AOCO/zstd
schema (`store_sales` range-partitioned by `ss_sold_date_sk` into 221
partitions), GPORCA, warm cache, two runs per setting; ten star-join queries
with selective dimension predicates:

| query | filters planned | filter off (ms) | filter on (ms) |
|---|---|---|---|
| q03 | 1 (`ss_item_sk`) | 407 / 399 | 310 / 299 |
| q07 | 1 (`ss_cdemo_sk`) | 755 / 679 | 540 / 536 |
| q27 | 1 (`ss_cdemo_sk`) | 803 / 810 | 646 / 640 |
| q55 | 1 (`ss_item_sk`) | 258 | 234 / 232 |
| q73 | 2 (`ss_hdemo_sk`, `c_customer_sk`) | 520 | 486 / 495 |
| q52 | 1 (`ss_item_sk`) | 235 / 260 | 226 / 236 |
| q19 | 2 (`ca_address_sk`, `c_customer_sk`) | 326 / 333 | 321 / 318 |
| q42 | 1 (`ss_item_sk`) | 239 / 233 | 242 / 233 |
| q43, q98 | 0 | unchanged | unchanged |

Here the filter leads the Staged Scan on the fact table. q03
(`i_manufact_id = 888`, 24 items):
`Dynamic Staged Seq Scan on store_sales ... Runtime Filter: ss_item_sk, Stage
Survivors: 1579 of 6988326 rows, Stage Blocks: 1482 decompressed, 748
skipped` -- 99.98% of the rows dropped on the key column alone, a third of the
other columns' blocks never decompressed. q07 (`customer_demographics`
filtered to 1.4%): `Stage Survivors: 81355 of 5773374 rows`, 6224 blocks
decompressed, 140 skipped. The whole query gains 20-25% because the scan is
followed by the date join, Motions and the aggregate, which the filter does
not touch. At this scale every hash table fits in cache, so the probe the
filter saves is cheap; the relative gain grows with the scale.

All 99 queries, each run once with the filter off and once on (filter off
first, so the on run has the warmer cache), on the final code: the planner
gave 57 of them a filter; the whole set took 292.8 s off and 267.2 s on
(-8.7%), the 57 with a filter 108.1 s and 83.5 s (-22.8%); but one cold off
run (q07, 17.1 s against its usual 0.7 s) inflates both totals, and without
it they read -3% and -9%. The robust figures are the median change per
query, -6.2%, and the counts: 31 queries more than 5% faster, 3 more than 5%
slower (q72 +10% at 22-24 s, q89 +9%, q93 +5%), all three within what the
queries *without* a filter -- identical plans -- showed between their two
runs (-32% to +15%). An earlier sweep, before the clause was moved behind
the scan's own quals and before a useless filter was rested, had a median
of -3.7% and
10 queries more than 5% slower; the same 57 queries got a filter both times.
Every query returned the same rows on and off; four (q64, q65, q71, q75)
listed them in another order, two of those with no filter in the plan: tie
order under `ORDER BY ... LIMIT`.

**TPC-DS, scale factor 30**, release build (no assertions), GPORCA, the same
schema (`store_sales` 86M rows in 221 partitions, an 11 GB database) and
cluster, on the branch's final code. Each query ran three rounds of one run
per setting, the two settings alternating within a round and the starting
side alternating from one query to the next, so that neither setting owns
the cold first touch of a query's data; the median of each setting's three
runs is the measurement, and every run is kept in the log. q23 and q95, the
two largest spillers, were left out (q95 runs out of temporary space on
this volume under both settings, q23 takes 100 s and gets no filter), and
q67 and q72 ran out of temporary space this time, with 5 GB free on the
volume, so they are not timed. 62 of the 95 timed queries got a filter. By
the medians: the 95 queries 313.2 s off, 280.5 s on (-10.4%); the 62 with a
filter 97.0 s and 73.8 s (-23.9%); the 33 without one -- identical plans --
216.2 s and 206.7 s (-4.4%). Per query with a filter: median -11.7%, 45
queries more than 5% faster, one more than 5% slower (q06, below); without
a filter the median change is 0.0% and the spread -28%..+10%, the ends
being q78, a 25-second query whose off runs varied by 60%, and q12, at
100 ms. The three runs of one setting differ by 10% at the median and by
12.9x at worst (the cold first off run of q62).

| query | filters | filter off, median (ms) | filter on, median (ms) | change |
|---|---|---|---|---|
| q50 | `ss_ticket_number` (one key of two) | 3151 | 1057 | -66% |
| q16 | 3 | 3461 | 1376 | -60% |
| q03 | `ss_item_sk` | 501 | 216 | -57% |
| q63 | `ss_item_sk` | 529 | 257 | -51% |
| q94 | 3 | 1741 | 878 | -50% |
| q90 | 2 | 1002 | 529 | -47% |
| q53 | `ss_item_sk` | 621 | 346 | -44% |
| q48 | 2 | 1290 | 739 | -43% |
| q96 | `ss_sold_time_sk` | 2135 | 1283 | -40% |
| q88 | 8 | 12611 | 7718 | -39% |
| q84 | 3 | 242 | 152 | -37% |
| q42 | `ss_item_sk` | 154 | 102 | -34% |
| q17 | 5 | 1362 | 914 | -33% |
| q52 | `ss_item_sk` | 159 | 108 | -32% |
| q29 | 3 | 1224 | 837 | -32% |
| q07 | `ss_cdemo_sk` | 934 | 634 | -32% |
| q55 | `ss_item_sk` | 152 | 107 | -30% |
| q64 | 7 | 10589 | 7425 | -30% |
| q80 | 5 | 1385 | 984 | -29% |
| q26 | `cs_bill_cdemo_sk` | 532 | 378 | -29% |
| q66 | 4 | 1031 | 752 | -27% |
| q82 | 2 | 1794 | 1306 | -27% |
| q27 | `ss_cdemo_sk` | 1044 | 761 | -27% |
| q24 | `ss_store_sk` | 9036 | 7199 | -20% |
| q21 | `inv_item_sk` | 159 | 141 | -11% |
| q89 | `ss_item_sk` | 810 | 734 | -9% |

q03 shows what the fast path changes: `Dynamic Staged Seq Scan on
store_sales ... Runtime Filter: ss_item_sk,
Stage Survivors: 17969 of 17066395 rows, Stage Blocks: 5000 decompressed, 244
skipped`. The 24 matching items leave a survivor in nearly every block, so
Phase B skips almost nothing, and the gain is Phase A's alone: 17M key
values hashed and probed off the datum array, 18k rows assembled. At scale
factor 10, with the clause evaluated row by row, the same query gained 25%.

One query with a filter is more than 5% slower by its median: q06 (231 to
244 ms), whose four filters remove little; its on runs of 234-244 ms
overlap its off runs of 222-243 ms. q93 (77 to 80 ms) looks for a return
reason the SF30 data does not have and returns no rows; its two filters
are built over no keys and have nothing to remove, so what shows is their
build. q21 (-11%) and q89 (-9%) gain least among the queries whose filter
leads a Staged Scan: theirs keep 6% and 11% of the rows of partitions a
dozen blocks long with a match in every block, and the Staged Scan's
first-batch check hands each segment file back to the ordinary scan after
Phase A has read it to its end.
Every query returned the same rows on and off; q24, q65, q71 and q73 list
them in another order, and q64, under `ORDER BY ... LIMIT 100` on columns
with ties, returned other tied rows within its hundred (without the LIMIT
its 582 rows are identical).

**The mode GUC, and tables never analyzed.** `whpg_runtime_filter_mode =
eager` plans a filter for every structurally eligible join, leaving out the
gates on probe rows and selectivity and letting the rows that arrive decide.
Measured against `estimate` on the same SF30 setup, statistics in place, two
interleaved rounds, the filter on under both: the 96 queries 281.8 s against
285.9 s (+1.5%). Eager adds filters to 72 of them, the joins the gates had
declined as too small or too unselective: those 72 take 224.9 s against
229.1 s (+1.9%), median change per query +0.0%, 9 queries more than 5%
faster (q15 -27%, 439 to 321 ms; q46 -23%, 3.65 to 2.81 s; q08 and q68
-14%; q73 -13%) and 10 more than 5% slower (q10 +22%, 641 to 785 ms; q64
+20%, 6.7 to 8.0 s; q78 +11%; q80 and q19 +10%); the 24 queries whose plans
are the same under both modes move -0.1%. With statistics in place, then,
the filters the gates decline are worth nothing in the whole, and the gates
spare the ten losses: `estimate` is the default. Without statistics -- the
`tpcds` schema's `pg_statistic` rows parked and its `reltuples` zeroed, as
a schema loaded and never analyzed -- GPORCA plans every query differently:
with no row counts it takes the dimension for the large side, so in q03,
q42, q52 and q89 the 86M-row fact is redistributed and hashed and the
73k-row `date_dim` is the probe, while q07, q19, q27, q34, q55 and q73 keep
the fact as the probe. Under `estimate` no query of the 99 gets a filter
(GPORCA puts every unanalyzed table at one row, so every probe falls under
the 10 000-row gate); under `eager`, and `auto` (eager where the probe's key
column has no statistics, which is everywhere here), 83 queries get 222
filters. Timed on twelve star-join queries, two interleaved rounds,
estimate against eager: 78.5 s against 64.9 s in total, but the difference
is two long queries' noise -- q52's first estimate run took 37 s against
11 s for its second, and q03 (-18%) and q89 (+15%), at 10 to 17 s a run,
have runs 2 to 4 s apart -- while the other nine move within 5% and the
median change per query is +0.9%. The filters eager adds to these plans sit
on a dimension probe or on a fact probed against a hashed table of unknown
size, and change nothing the noise does not hide; what a never-analyzed
schema needs under GPORCA is ANALYZE, not a filter. The Postgres planner without
statistics sizes a table from its file, so its estimates keep the right
side large, but it plans a partitioned fact as an Append of partition
scans, which the pass does not reach yet (follow-up), so the TPC-DS facts
get no filter under it in either mode.

**The fast path alone.** A 12M-row AOCO fact of 8 columns joined to a
120k-key dimension, 1% of the fact's keys, clustered at the start of each
segment file so that Phase A runs to the end (`Stage Survivors: 121577 of
12000000 rows, Stage Blocks: 2437 decompressed, 3041 skipped`); the same join
once on the key column, whose blocks are NULL-free and take the fast path,
and once on a copy of it with one NULL per 4000 rows, which sends every block
down the row-by-row path. Five rounds each, release build, same binary:

| key column | filter off | filter on |
|---|---|---|
| NULL-free (fast path) | 285-310 ms | 89-92 ms |
| one NULL per block (row by row) | 357-384 ms | 250-259 ms |

The NULL-bearing copy costs the ordinary scan some 75 ms more on its own, so
the clause's own cost over 12M rows comes to about 90 ms on the fast path
against about 180 ms row by row; the filtered scan is 2.8x faster with the
fast path than without it, and 3.3x faster than the unfiltered scan.

Two of the TPC-DS queries (q58, q83) join on a `date` key, which the hash
opfamily hashes with `hashint4`: the key's own type is not what the hash
function declares. The clause carries the hash operator, and the one check of
a key against it (`runtimefilter_key_hashable()`, shared by the planner pass,
the probe and the Staged Scan) is against the operator's left-hand operand
type, as `ExecHashTableCreate()` pairs them, so such keys get a filter.

## Toward cross-segment filters

The form implemented here is the first phase: the fuller feature lets a filter
also reach a probe scan on the far side of a Redistribute Motion, so that the
rows are dropped before they are sent, saving the network as well as the probe.
That design weighs two ways of getting the filter across -- at the plan level,
rewriting the plan so that the filter is aggregated and broadcast as a tuple
that a NestLoop injects as a Param, or at the executor level, a downstream
channel in the interconnect from the join's slice to the scan's -- and in either
the scan side is the same. What is here is built to be that scan side:

- **The hand-off is an exec Param holding a bytea.** Whatever produces the
  filter -- this join, a NestLoop's `nestParams` from a broadcast tuple, or a
  receiver in the scan's slice -- sets `es_param_exec_vals[p]` and the scan
  reads it through the same clause. The clause reads the Param on every row,
  so a filter that arrives after the scan has started takes effect from the
  next row, and a NULL Param (not arrived, dropped, timed out) means "no
  filter". The Staged Scan's Param plumbing works for any producer.
- **The filter format is mergeable and self-describing.** The bytea carries
  a magic, the block count and the key count; two filters of the same block
  count merge by OR (`runtimefilter_merge()`), which is what a segment-wide
  filter is: the OR of per-segment filters over the same hash values. Block
  counts agree when every builder sizes from the same plan estimate, as they
  do. Nothing in it is specific to one process.
- **The hash values are the hash table's.** They are the opfamily's hash
  functions, deterministic across segments, so a filter built on one segment
  is valid for probes on another; a partitioned filter (one per destination
  segment of the probe's Motion) would be built from the same values.
- **The planner pass is a post-processing pass over the finished plan**, for
  both optimizers, which is where the plan-level design wants its rewrite as
  well. `rf_find_probe_scan()` reports a Motion as its own outcome
  (`RF_PROBE_MOTION`), distinct from the shapes that get no filter; that
  branch in `rf_consider_hashjoin()` is where the cross-segment version
  chooses its transport and continues to the scan on the far side.
- **`HashJoin.rf_paramids` is a list.** One entry today; the cross-segment
  join would carry the Params of the filters it feeds, local and remote, and
  the publish step (`ExecHashJoinPublishRuntimeFilter()`) is the one place
  that would also serialize and send.
- **Sizing is reproducible.** Every builder sizes its filter from the plan's
  estimate and `whpg_runtime_filter_max_size`, which is synchronized to the
  segments, so filters built for the same join on different segments have
  the same block count and merge; the header's `flags` word (0 today, equal
  on both sides of a merge) is where a partitioned or otherwise special
  filter will say so.
- **The scan side needs no new contract to wait.** A filter that arrives
  after the scan has started applies from the next row; a scan that should
  rather wait for it -- the design's `wait_timeout` -- would do so where the
  scan is opened (`SeqNext()`), before the first row, with the Param still
  NULL meaning "go ahead without it".

What the cross-segment work adds is therefore the transport (a plan rewrite or
an interconnect channel), the aggregation of per-segment filters, the wait /
timeout policy at the receiving scan, and the planner's decision of when the
network saving pays for the transport; the filter, the clause, the scan side
and the EXPLAIN stay.

## Limitations and follow-ups

- The survivor estimate, the join's output rows over the probe's rows, is an
  upper bound: a one-to-many join (duplicate keys on the hash side) can put
  it over the selectivity gate although few probe rows have a match, and
  then no filter is planned. A distinct-key estimate for the inner side
  would tighten it.
- A join on several keys gets a filter over one of them. A filter over the
  combined hash value (what the table uses) would be tighter where no key
  alone is selective -- the pairs of two unselective keys can be rare -- once
  the clause takes a key list; it would drop probe rows a single key's filter
  passes, at the price of hashing every key of every probe row.
- The per-key estimate behind the choice assumes the inner side's keys are a
  subset of the probe's (a foreign key's), spread evenly, and takes the inner
  side's distinct values as its rows when the key is not a column with
  statistics; a selective predicate on the probe scan's other columns leaves
  it optimistic (the probe's distinct values are those of the whole column).
  The probe's watch on the filter covers what the estimate misses.
- Under the Postgres planner a partitioned probe gets no filter: it is an
  Append of the partitions' scans, and the pass stops at the Append. The
  clause would go on each child scan, with the key translated to the
  partition's column (partitions may lay their columns out differently).
  GPORCA's DynamicSeqScan is covered, so a query loses its filter when
  GPORCA falls back to the Postgres planner.
- The raw-datum fast path is Phase A's. Once the first-batch check hands a
  segment file back to the ordinary scan, and on heap and AO row tables, the
  clause runs through `ExecQual()` and a function call per row; a batch probe
  for the ordinary scan's qual is what the dense case above still needs.
- That check judges a filter-led Staged Scan as it judges any other: a
  segment file whose first batch keeps more than a tenth of the rows, or a
  match in nearly every block, goes to the ordinary scan, which reads again
  what Phase A has read. On short partitions that is the whole file.
- Only the key column's own blocks take the fast path; a key of a type outside
  the integer and datetime families (text, numeric, uuid) goes row by row.
- The filter's keys and size are not yet reported by EXPLAIN ANALYZE.
