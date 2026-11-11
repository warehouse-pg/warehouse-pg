# Staged Scan: two-phase scan for AOCO tables

Staged Scan is not an index. It is a way of running a sequential scan over an
append-optimized, column-oriented (AOCO) table when the predicate is selective:
read the filter column first, find the rows that survive, then fetch the other
columns only for those rows, decompressing only the varblocks that hold one.
The technique is the late materialization of the column stores (C-Store,
Impala, ClickHouse's PREWHERE); the implementation here is
WarehousePG's own.

Code:

| piece | file |
|---|---|
| storage primitive: header-only read, content read, skip, seek, fixed-width positioning | `src/backend/utils/datumstream/datumstream.c` |
| Phase A, survivor array, clause-to-stage split, applicability, adaptive reordering | `src/backend/access/aocs/aocs_stagedscan.c` |
| Phase B, per-row staging, OR bypass, executor entry point | `src/backend/access/aocs/aocsam.c` (`aocs_stagedscan_getnext`) |
| AM hook, decision handover | `table_beginscan_es_staged()` in `tableam.h`, `src/backend/access/aocs/aocsam_handler.c` (`aoco_beginscan_extractcolumns`) |
| cost model and stage ordering | `src/backend/optimizer/path/stagedscancost.c` |
| planner hook | `create_seqscan_plan()` in `createplan.c` |
| plan fields | `SeqScan.staged.enabled`, `SeqScan.staged.filterAttnos` in `plannodes.h` |
| EXPLAIN | `show_staged_scan()` in `explain.c` |
| GUCs | `guc_gp.c`, listed in `sync_guc_name.h` |
| tests | `staged_scan_datumstream_split`, `staged_scan_aoco`, `staged_scan_fastpath`, `staged_scan_planner`, `staged_scan_or`, `staged_scan_ordering`, `staged_scan_orca` |
| measurement scripts | not in the tree; the headings of the measurement sections below name the script each table came from (`bench_scale.sql` for the SF100 sweep, `bench_adaptive_sf100.sql`, `headline.sql`, `gen_tpch.sh`/`load_tpch.sh` for the TPC-H data, and the 6M-row development sets) |

## Contents

- [What the ordinary scan pays](#what-the-ordinary-scan-pays)
- [How Staged Scan runs the scan](#how-staged-scan-runs-the-scan)
  - [Multi-column predicates are staged inside Phase B](#multi-column-predicates-are-staged-inside-phase-b)
  - [A Phase A wider than one column](#a-phase-a-wider-than-one-column)
  - [What a Staged Scan guarantees](#what-a-staged-scan-guarantees)
  - [The decision is the planner's, and that is measurable](#the-decision-is-the-planners-and-that-is-measurable)
  - [Cost model](#cost-model)
  - [Ordering the stages: clause orders, not column permutations](#ordering-the-stages-clause-orders-not-column-permutations)
  - [OR clauses inside Phase B: bypass](#or-clauses-inside-phase-b-bypass)
  - [Adaptive ordering inside the scan](#adaptive-ordering-inside-the-scan)
  - [Worked examples: one model for AND, OR and mixed predicates](#worked-examples-one-model-for-and-or-and-mixed-predicates)
  - [What is declined, and why](#what-is-declined-and-why)
  - [GPORCA](#gporca)
  - [Why no block-directory gate](#why-no-block-directory-gate)
  - [Seeking through the block directory](#seeking-through-the-block-directory)
  - [Phase A on the raw datum array](#phase-a-on-the-raw-datum-array)
  - [EXPLAIN ANALYZE and "Rows Removed by Filter"](#explain-analyze-and-rows-removed-by-filter)
  - [Binary compatibility](#binary-compatibility)
- [GUCs](#gucs)
- [Tests and fault injectors](#tests-and-fault-injectors)
- [Measurements](#measurements)
  - [WarehousePG](#warehousepg)
  - [Realistic shapes (`bench_suite.sql`)](#realistic-shapes-bench_suitesql)
  - [OR bypass (`bench_suite.sql` group `bypass`)](#or-bypass-bench_suitesql-group-bypass)
  - [Adaptive reordering at SF100 (`bench_adaptive_sf100.sql`)](#adaptive-reordering-at-sf100-bench_adaptive_sf100sql)
  - [TPC-H SF100 on the optimized build (`bench_scale.sql`)](#tpc-h-sf100-on-the-optimized-build-bench_scalesql)
  - [Earlier measurements (development build)](#earlier-measurements-development-build)
- [How other column stores order predicates, and the references](#how-other-column-stores-order-predicates-and-the-references)

## What the ordinary scan pays

`SELECT a, b, c FROM t WHERE d = X` on an AOCO table advances four datum
streams in lockstep. Every row costs a `datumstreamread_advance()`/`_get()` per
column, and every varblock of every column is decompressed, before the qual
throws the row away. For a predicate keeping 1% of the rows, 99% of that work is
wasted.

## How Staged Scan runs the scan

Phase A opens its own `DatumStreamRead` on the first filter column, reads every
row, evaluates that column's clauses, consults the visimap, and appends the
surviving row numbers to an ascending `int64` array. It does so in batches:
the first batch holds 8 survivors, each later one twice the previous, up to
`STAGEDSCAN_BATCH_MAX_ROWS` (65536, half a megabyte); after each
batch Phase A pauses with its streams open, Phase B consumes the batch, and
Phase A resumes where it stopped until the segfile is exhausted. Successive
batches are ascending like the rows within one, and every Phase B cursor only
moves forward, so batches are invisible to Phase B; what they buy is memory
bounded by the largest batch rather than by the segfile's hit count, so a
table of any size and a predicate of any hit count run through the same
path. The small first batch bounds the time to the first row: a `LIMIT`, an
`EXISTS` or a cursor fetching a few rows gets its rows once Phase A has found
8 matches instead of 65536 (which on a sparse predicate could mean the whole
segfile), while batch switches cost nothing measurable, so a full scan is
unaffected. The first batch of a single-stage predicate is also the sample
for the fallback checks below and keeps filling until Phase A has crossed 64
blocks, whatever its size (or until the array is full): on a sparse predicate
those blocks pass in milliseconds, on a dense one they hold the evidence for
falling back. (An earlier version collected a whole segfile's survivors first and fell
back to the ordinary scan past a fixed cap, which on large tables turned the
most profitable scans into the slowest ones.)

Phase B hands out the survivors one at a time. For each row and each column it
needs, `stagedscan_fetch_column()` walks the column's segfile by varblock
*header* (`firstRowNum`, `rowCount` are in the header) with
`datumstreamread_block_header_only()`, steps over blocks whose row range ends
before the wanted row with `datumstreamread_block_skip_content()`, and only
calls `datumstreamread_block_content()` on the block that holds it. Because
survivors ascend and every column shares the row numbering, each column's
cursor only ever moves forward, and once the survivor array is exhausted the
tail of every segfile is never touched.

Two things this buys, and only one needs clustering:

- **Block skipping** needs the survivors to be clustered in row-number order
  (which on an append-optimized table is insertion order). It saves the
  decompression, checksum and datum-unpack work of the skipped blocks. The
  header walk alone does *not* save their I/O: `AppendOnlyStorageRead_SkipCurrentBlock()`
  still pulls the bytes through the buffered-read layer, and the only genuine
  I/O saving is the segfile tail past the last survivor. That is what the
  block-directory seek below adds.
- **Fewer datum unpacks** needs only selectivity. Each projected column is
  advanced only to the survivors instead of to every row. A predicate that is
  scattered enough to skip zero blocks still measured 18-33% faster from this
  alone.

### Multi-column predicates are staged inside Phase B

`WHERE d = X AND e = Y`: Phase A runs on one column only; the *stage order* is a
planner decision. For each survivor, Phase B fetches `e` first, evaluates
`e = Y`, and drops the row before touching any projected column if it fails.
Since every column is fetched through the same skip-by-header routine, `e`
skips blocks against Phase A's survivors and the projected columns skip blocks
against the rows that passed both clauses. No second pass, no survivor-array
rewriting: the staging is the order in which one row's columns are fetched.

A clause is decided at the *last* stage among the columns it references, so a
clause spanning two columns (`a + b > 10`) needs no special case. Stage 0 must
have at least one clause or Phase A would keep every row.

### A Phase A wider than one column

A clause that needs several columns before it can drop a row -- `d = X OR
e = Y`, or `a + b > 10` -- cannot lead a one-column Phase A. Rather than
decline, the planner may widen Phase A to the first k columns of the stage
order (`SeqScan.staged.phaseAWidth`); the executor then opens one stream per
Phase A column and advances them in lockstep, row by row, exactly as
`aocs_getnext()` advances the projected columns, and evaluates the clauses
confined to those columns. Every column added to Phase A costs a full pass,
which a later stage never does, so for an AND-only predicate the cost model
always settles on k = 1 and nothing changes; k > 1 is chosen only when the
multi-column clause is the cheapest thing that can lead. `(d = X OR e = Y)
AND f = Z` therefore usually keeps f as a one-column Phase A and decides the
OR at a later stage, while a bare `d = X OR e = Y` reads d and e together.
EXPLAIN shows the Phase A columns first and the stages after them,
`Stages: (d, e), f`.

### What a Staged Scan guarantees

The contract is that of the plan without it: the same rows with the same
column values, and no error from evaluating the qual on a row the ordinary
scan of the same plan would not evaluate it on. The ordinary scan
evaluates the plan's qual in list order on each visible row and stops at the
first clause that is not true. (How often a non-volatile clause runs is not
part of it, as in PostgreSQL: after a fallback the ordinary scan evaluates
the first batch's rows again. Nor is how far ahead of a stopping consumer
the scan has read: under a `LIMIT` Phase A finishes its batch, as every
segment of an MPP plan already scans ahead of the coordinator's `LIMIT`.) A Staged Scan evaluates the same clauses in its
own order, on rows it has not finished assembling, so three rules carry the
contract. Each has one enforcement point, and the first and third are checked
per row in assert-enabled builds.

1. **A clause reads only columns fetched for the current row.** The filter
   slot the stages evaluate is reused from row to row, so a column not yet
   fetched for this row would silently hold the previous row's value. A
   Phase B stage column is fetched at its own stage and a clause is decided
   no earlier than its last column's stage. A Phase A column (whose datums
   are gone after Phase A) is fetched at the first stage, in stage order, that
   reads it, settled before any clause is placed, since the qual's own order
   says nothing about stages. A column only OR clauses read is skipped only
   when every one of them is settled for the row, and from then on nothing
   reads it (`stagedscan_build_stages()`; check: `filterStamp`).
2. **A clause that can fail is shown only the rows the ordinary scan shows
   it.** "Can fail" means it calls anything not marked leakproof, or contains
   a node type that might fail (`stagedscan_clause_can_fail()`): a cast from
   text, integer arithmetic, division, `LIKE`, numeric comparison. A
   leakproof function promises exactly that its outcome cannot depend on
   anything but its result, so a clause made only of those may be evaluated
   anywhere. A clause that can fail must be decided no earlier than every
   clause before it in the plan's qual, at a later stage or after them within
   its stage (`stagedscan_order_safe()`), so that it too sees only rows they
   all passed. The planner considers only such orders, the executor declines
   a plan whose order breaks the rule whoever built it
   (`stagedscan_applicable()`), and the run-time reordering does not make a
   swap that breaks it. An OR clause that can fail is not split across stages
   (its disjuncts run left to right as in the ordinary scan), and when Phase A
   evaluates such a clause it consults the visimap first: a row deleted for
   holding bad data is the row a cast fails on. Row-level security falls out
   of the same rule, since `order_qual_clauses()` puts a policy's clauses
   ahead of the user's leaky ones.
3. **Every column the scan's consumers read is in every emitted row.** The
   target list's columns (and the constraint list's) are fetched for every
   row that passes, whichever stage or the final projection fetches them. A
   column only the qual reads may be left unfetched (NULL) in the emitted
   slot, which is safe because the scan node runs no qual of its own exactly
   when the access method owns it (`aocsam_handler.c`, `nodeSeqscan.c`;
   check: `outStamp`).

Rows themselves come from Phase A's survivor set: visible rows only, ascending,
each once, batches resuming where the last stopped, and a fallback only before
a segment file's first row is emitted.

The price of rule 2 is reordering freedom: a clause that can fail leads
Phase A only from the front of the qual, and two such clauses keep their
relative order. `f = 7 AND f + e >= 8 AND f + d = 12` is staged `f, e, d`
even though `f + d = 12` is the more selective, because integer addition can
overflow. Leakproof clauses (integer, date and timestamp comparisons, text
equality) still move freely, and they are what usually leads.

### The decision is the planner's, and that is measurable

An earlier version decided in the executor. It could only discover an
unselective predicate by counting survivors, and by then it had scanned and
decompressed part of the filter column for nothing: 5-18% slower than never
having tried, across the 11-40% selectivity band. Moving the decision into
`create_seqscan_plan()` removed the whole regression band. The executor treats
the plan flag as permission, not instruction: it re-checks the preconditions
only it can see (AOCO relation, no missing values in the filter column, a plain
forward seq scan, the stage columns and the qual's columns being the same set)
and falls back to `aocs_getnext()` silently. Falling back is only possible
while no row of the segment has been emitted, that is before the first
survivor batch is handed over, because it hands the segment to
`aocs_getnext()` with the projected streams still on their first block. The
one data-dependent fallback left, the single-stage selectivity re-check below,
is therefore made on the first batch.

The scan node runs no qual of its own on a Staged Scan plan. `ExecInitSeqScan`
sees the plan flag on an AOCO relation and initializes the node without a
qual, and the access method, asked to open the scan as a staged scan,
owns the predicate from then on (`AOCSScanDescData.stagedQualOwned`): Phase A
and the stages decide every clause, so the two-phase path returns only rows
that satisfy it, and every other path filters -- when Staged Scan is
declined at open, and once a segment file has been handed to
`aocs_getnext()`, the rows the ordinary scan yields are tested against the
whole qual inside the access method (`stagedscan_fallback_getnext()`) and the
rejected ones counted as removed by the filter. A partition of another
access method under a dynamic scan keeps the node's qual. Two things follow.
A column only the predicate reads -- the Phase A column, most often -- is
not fetched for the emitted row at all unless the target list or a later
stage wants it; and a plan whose qual referenced a column no stage decides
would go unenforced, so `stagedscan_applicable()` declines it (the planner
never produces one) and the ordinary scan with the filter runs instead.

### Cost model

Writing C for the projected columns, f for the filter column, N rows, sel
selectivity, S = sel·N, B(c) a column's varblock count (its width times N
over its own block size, from `pg_attribute_encoding`), dpb(c) its
per-varblock decompression cost, R(c) its I/O, U the per-datum unpack cost:

```
seq   = Σ_C [ R(c) + D(c) + N·U ]
staged = [ R(f) + D(f) + N·U ]                                        Phase A
      + Σ_C [ ρ·R(c) + H(c)·dpb(c) + S·U + T(c)·U + ρ·B(c)·W ]       Phase B
```

`H(c)` is the number of varblocks holding at least one survivor and `ρ` how deep
into the segfile Phase B gets before the survivor cursor is exhausted. Both
depend on how clustered the matching rows are in row-number order, which is
exactly `STATISTIC_KIND_CORRELATION`, so both interpolate on correlation
squared between a contiguous band and uniform scatter -- the same idiom
`cost_index()` uses for its AO decompression charge. With no statistics the
correlation defaults to 0: no block skipping is claimed and Staged Scan has to
justify itself on the per-datum saving alone.

The last two Phase B terms were found on the optimized build at TPC-H SF100,
where the ordinary scan's own per-row work is small enough for them to show.
`T(c)` is the datums Phase B advances past inside the hit blocks: positioning
in a block is `datumstreamread_find()`, datum by datum from where the stream
stands, so a block holding s survivors pays about rpb·s/(s+1) of the unpack
work the ordinary scan pays for the whole block -- half a block for a lone
survivor, all of it once the block holds many. `W` is the per-block walk
overhead (header read, content skip, cursor bookkeeping), charged like one
decompression; it is not a measured primitive but the fitted constant that
puts the accept/decline boundary where the sweep measured it. With survivors
in nearly every block T approaches N and W is pure overhead: such predicates
measured no gain (0.1% and up scattered over lineitem), while fewer than one
survivor per block still gained about 50% and clustered bands 60-90%.

The correlation statistic has a blind spot the model must live with: a table
loaded by several parallel sessions holds several interleaved ascending runs
per segment, its correlation is near 0, and yet a band of key values sits in
a few blocks per run (a 1% `l_orderkey` band on a gpfdist-loaded lineitem:
correlation -0.09, 70% faster with Staged Scan, declined by the pessimistic
estimate). A single-stage predicate the estimate declines is therefore
re-costed as if its column were perfectly clustered, and when that says yes
the executor gets the scan and decides on evidence: the first batch of
survivors shows how many of Phase A's blocks they occupy, and Phase A falls
back to the ordinary scan before any row is emitted if they are dense
(`stagedscan_survivors_too_dense()`, the same per-column estimate on observed
h, s and rows per block, judged once at least 64 blocks have been crossed so
that a dense band is not mistaken for a dense table, falling back when Phase
B is estimated at 90% or more of the ordinary scan). Only a single stage can
fall back, which is why the optimistic retry is limited to one filter
column. A first batch with no survivor at all is never dense: Phase B has
nothing to walk. Plain EXPLAIN therefore shows `Staged Seq Scan` for some
plans the executor hands back on their first batch: it shows what the plan
permits, and EXPLAIN ANALYZE what ran (`Stage Fallback`). Gating the retry on
the correlation statistic would lose exactly the interleaved-runs case it is
for; a fallback costs one batch of Phase A.

For k >= 2 filter columns the survivor set is modelled as scattered inside a
band of the file that every clustered clause narrows; the k = 1 formulas are its
endpoints. The candidate orders come from the predicate's structure (next
section) and each is costed end to end; "most selective first" is not the
rule, because Phase A pays a full pass over its column, so a narrow 10% column
can beat a wide 0.4% one as Phase A. When the target list wants the Phase A
column as well, a single by-value column (integers, dates, timestamps,
floats, booleans) is carried with the survivors -- Phase A has the datum in
hand when it records the row -- and costs Phase B nothing; a
pass-by-reference column (text) or a Phase A wider than one column is walked
again in Phase B, and that second walk is charged no I/O (the file was just
pulled through the page cache). Without that correction the model led with
the wrong column on one measured shape and lost 18%. A Phase A column the
target list does not want is fetched by no one (the scan node rechecks
nothing, see above).

The whole predicate must be estimated at or below
`whpg_staged_scan_min_selectivity` (default 0.10). For a single stage the
executor re-checks the same bound on the first batch of survivors -- if the
rows Phase A has scanned so far kept more than that fraction of themselves,
the statistics were wrong and the ordinary scan is the better deal -- and
falls back before emitting a row, and the density check above falls back the
same way. Both judge the first batch only once it covers 64 blocks or the
whole segment file. The array bound can close it sooner, on a clustered band
at the start of the file as on a predicate that keeps nearly every row, and
the rate over a few blocks cannot tell the two apart; the count can. So a
batch closed by the bound is judged only if its 65536 survivors are more than
four times what the planner estimated for the whole segment file: the
statistics are then wrong wherever the remaining matches lie, while a band
is what they predicted, and it stays staged. Later batches never fall back, the scan is correct either way and
merely less profitable; so a segment file whose matches are sparse in its
first blocks and dense after them is staged throughout. For k >= 2 the gate is not re-checked against Phase
A alone, because a 30% × 30% predicate whittled to 9% by stage 2 is a
measured 24% win; what is checked is the planner's own estimate of Phase A's
selectivity, which travels on the plan (`SeqScan.staged.phaseASel`): if the
first batch (judged by the same rule) shows Phase A keeping more than four
times that estimate and more than twice the gate, the statistics the stage
order rests on were wrong -- a clause on an expression the statistics know
nothing about, `a % 2 = 0`, is estimated at 0.5% whatever it keeps -- and
the segment falls back before emitting a row (fault point
`StagedScan_PhaseA_EstimateFallback`). Smaller misses are left to the stages
and to the run-time reordering. Nothing else limits the hit count.

### Ordering the stages: clause orders, not column permutations

A clause is decided only when the last of its columns is in hand, so the
choice that matters is the order in which the *clauses* are decided; the
column order follows from it. The planner therefore enumerates clause orders
(every one of them up to `STAGED_MAX_ENUM_CLAUSES` = 6 clauses, 720 candidates;
beyond that only increasing estimated selectivity) and expands each into a
column order: clause by clause, each appends the columns it still needs. The
first clause's columns form Phase A (`staged.phaseAWidth`), which is why an AND
of single-column clauses always gets k = 1 and a leading OR gets exactly its
columns. A column two clauses share is placed once, by the first clause that
needs it. An OR clause appends its columns disjunct by disjunct, the disjunct
with the highest estimated chance of settling a row per unit of column cost
first, so that the executor's bypass (below) has the most to gain. For the
predicate `f = 7 AND (s = 1 OR d = 5)` with a rarely true `f`, a usually true
`s` and a rarely true `d`, the chosen order is `f, s, d`.

The candidates share their prefixes, so the cumulative selectivity of each
set of clauses is estimated once and remembered (at most 63 sets for six
clauses), and everything the decision allocates -- extended statistics
included, which the estimator deserializes on every call -- is freed when it
returns: the Postgres planner makes the decision once per partition of a
partitioned scan.

A candidate that would decide a clause able to fail ahead of a clause the
qual evaluates before it is not costed at all (rule 2 of
[What a Staged Scan guarantees](#what-a-staged-scan-guarantees)), which is
why `create_seqscan_plan()` hands the model the clauses in the qual's order.

The earlier scheme permuted the columns themselves, at most four of them, and
tried every Phase A width for each permutation. Permuting columns explores
orders that no clause order produces (splitting an OR's columns around an
unrelated column buys nothing) and its bound was on columns rather than
clauses, so a two-clause predicate over five columns fell out of the
exhaustive search. Ranking clauses by rank = (selectivity - 1) / cost, the
classical result, is optimal only for independent predicates of constant cost
(Kastrati & Moerkotte 2016); here a stage's cost depends on how many rows and
varblocks the stages before it let through, which is why candidates are costed
end to end rather than ranked.

Selectivities along a candidate are estimated cumulatively: the clauses
decided up to and including a stage are handed to `clauselist_selectivity()`
together and the stage's own selectivity is the ratio to the previous total.
Handed the clauses together, the estimator applies extended statistics
(`CREATE STATISTICS ... (dependencies, mcv)`) and its own handling of several
clauses on one column, so a clause implied by an earlier one is seen to reject
nothing and is not worth deciding early. On the `staged_scan_planner` table
where `b` is a function of `a`, `a = 7 AND b = 0 AND c = 1` is staged
`a, b, c` from single-column statistics (b looks 1%-selective) and `a, c, b`
once a dependency statistic on `(a, b)` exists. This is the plan-time answer
to correlated predicates, the same problem the run-time reordering guards
against; keeping ANALYZE current and declaring the dependencies the data has
lets the planner get the order right before a single row is read.

### OR clauses inside Phase B: bypass

An OR clause decided in Phase B is not evaluated as a unit. Each disjunct is
compiled on its own and evaluated at the first stage that has all of its
columns; a true disjunct settles the clause for the row, and a column that only
OR clauses need is not fetched for a row on which every one of them is already
settled (`StagedScanStage.fetchNeededBy`). An OR clause still open when its
last disjunct has been evaluated rejects the row. NULL semantics are those of
the original clause: a NULL disjunct settles nothing, so `NULL OR true` is kept
and `NULL OR false` dropped. A column left unfetched this way is stored as
NULL in the emitted row; nothing reads it, since the scan node runs no qual
on a staged scan. An OR clause that can fail is the exception and stays a
unit: splitting it could evaluate a disjunct the ordinary scan's
left-to-right evaluation never reaches (`f = 7 AND (y = 0 OR 100 / x > 30)`
must not divide where `y = 0` already settled the row).

For `f = 7 AND (s = 1 OR d = 5)` on the test table (`staged_scan_ordering`):
301 rows pass `f = 7`, 270 of them are settled by `s = 1` without ever fetching
`d`, the other 31 fetch `d` and are rejected. With the bypass off the same 31
are rejected, no row is bypassed, and `d` is fetched for all 301.

The planner knows about the bypass: the fetch cost of a column only OR clauses
need is scaled by the probability that some earlier disjunct of every such
clause has not already settled the row, the product of (1 - p_j) over the
disjuncts evaluable at earlier stages (independence assumed). The disjunct
ordering and this fraction are the planner-side half of the bypass. An OR
clause that can fail (see "What a Staged Scan guarantees") is a unit on both
sides.

This is the set-management idea of Kim, Ileri and Madden (EvalPred) reduced to
its per-row form: the survivor set is not rewritten per disjunct, each row
simply carries which clauses are still open (`clauseDecided`), and the
staging order does the rest.

### Adaptive ordering inside the scan

The planner's order rests on statistics, and the data can disagree. Each
Phase B stage counts, per window, the rows that reached it, the rows it
rejected, the rows on which it settled an OR clause and the varblocks it
decompressed. When a window closes the executor scores each stage as
(rejected + 0.25 · settled) / (rowsIn + 16 · blocksDecompressed) -- the
rank ordering with the cost measured rather than estimated -- and makes one
bubble pass over adjacent stages, swapping a pair whose later stage beats the
earlier one by more than 25%. A stage that saw fewer than
`STAGED_ADAPT_MIN_STAGE_ROWS` rows (1000) in the window is
never part of a swap (the window said nothing about it), two adjacent stages
that evaluate disjuncts of the same OR clause are never swapped with each
other (the disjunct order stays the planner's), the Phase A prefix never
moves, and a swap that would decide a clause able to fail ahead of a clause
the qual evaluates before it is not made (rule 2 of
[What a Staged Scan guarantees](#what-a-staged-scan-guarantees)). If anything changed the stage plan is rebuilt from the new order
(`stagedscan_build_stages()` in `stageCtx`), which recomputes which stage
settles which clause, so the results cannot change; only the order in which a
row's columns are fetched does.

Windows are counted in survivors entering Phase B, which is where every cost
and every observation of Phase B arises. The first window closes after
`STAGED_ADAPT_INITIAL_WINDOW` survivors (4096) and each
later window is twice the previous; the end of a segfile closes the current
window too. Most tables have one or two segfiles per segment, so re-scoring
only at segfile boundaries -- the first version of this -- learnt the right
order when there was little or nothing left to apply it to; with windows a
wrong order is corrected after a small prefix of the segfile and a settled
order is re-checked ever more rarely. The counters are zeroed at every check,
so each window's scores describe the order actually in force during it: a
swap made on one window's numbers is confirmed or undone by the next
window's own measurement. That is also what keeps the classical caveat of the
rank ordering -- two stages' rejection rates are only comparable if they are
independent -- from mattering much: a swap that the data does not bear out is
reversed one window later. Reordering between two rows is safe because a
stage order only decides in which sequence one row's columns are fetched;
every column's cursor moves forward on its own, whatever the order.

What a reorder is worth follows from what Phase B pays: a misplaced narrow
column costs one datum unpack per row, a misplaced wide column costs the
decompression of every varblock a survivor falls into. On the 6M-row
benchmark a wrong order among int columns (600k survivors, all blocks
holding survivors anyway) cost nothing measurable, corrected or not; see
"Measurements" for the wide-column case the windows were built for.

This is the shape of Babu et al.'s A-Greedy (2004) with the profile window
replaced by a window of survivors and the full-evaluation sample replaced by
the counts the pipeline produces anyway.

### Worked examples: one model for AND, OR and mixed predicates

The rules above are the whole model: the predicate is a set of top-level AND
clauses; the planner picks a stage order and a Phase A width k; a clause is
decided at the last of its columns; Phase A reads its k columns in full and
produces the survivors; every later stage fetches one column for the rows
still alive; an OR clause is additionally settled by its first true disjunct.
On the `staged_scan_ordering` test table (200k rows; `f = 7` on a 301-row
band, `d = i % 1000` and `e = i % 997` scattered, `s = 1` on nine rows in
ten, `g = 'g5'` on four rows, `pad` a wide md5):

| predicate | stage order | how a row goes |
|---|---|---|
| `f = 7` | `f` | Phase A reads f in full, 301 survivors; Phase B fetches the projected columns for those alone. |
| `f = 7 AND d = 5` | `f, d` | For each survivor d is fetched and `d = 5` evaluated first; 300 rows stop there, one row reaches the projected columns. d skips blocks against 301 rows, the projection against one. |
| `f = 7 AND d = 5 AND g = 'g5'` | `f, g, d` | Three single-column clauses, six clause orders costed; g keeps four rows and is cheaper to decide before d. |
| `d + e = 1000` | `(d, e)` | One clause over two columns: neither can drop a row alone, so k = 2 and Phase A reads both in lockstep. |
| `d = 5 OR e = 5` | `(d, e)` | A leading OR: a row can only be dropped once both are known, k = 2, about 400 survivors. Same I/O as a bitmap per column OR-ed together. |
| `f = 7 AND (s = 1 OR d = 5)` | `f, s, d` | f leads (301 rows). At stage s, `s = 1` holds on 270 rows: the clause is settled and d is not fetched for them. The 31 rows with `s = 0` fetch d, none has `d = 5`, all are rejected. Injector counts: 301 survivors, 270 bypassed, 31 rejected. |
| `d = 5 AND (s = 1 OR pad = 'x')` | `d, s, pad` | Every one of the 200 survivors has `s = 1`; the wide pad column is never fetched. With the bypass off it is fetched 200 times and the stages decompress more blocks. |
| `f = 7 AND (d = 5 OR e = 5) AND d < 100` | `f, d, e` | d is shared by the OR and a plain clause: placed once, and always fetched because the plain clause needs it; e is only the OR's, and rows with `d = 5` never fetch it. |
| `f = 7 AND (d = 5 OR s = 0 OR e = 5)` | `f, s, e, d` (d and e tie) | Three disjuncts over three stages: `s = 0` settles a tenth of the rows at once, the rest fetch one of d and e, then the other; the last disjunct's stage rejects what is still open. |

Three fixed points: a projected column is always fetched (the bypass then
saves only the evaluation); a column a non-OR clause needs is always
fetched; a column two OR clauses need is skipped only once both are settled.

How the `f, g, ...` order of `(d = 5 OR e = 5) AND f = 7 AND g = 'g5'` came
out of the clause-order enumeration (d and e have the same selectivity and
width, so which of them comes third is a tie):

| clause order | expanded column order | k | why |
|---|---|---|---|
| OR, f, g | `(d, e), f, g` | 2 | Phase A reads two columns in full, and f and g only see the OR's ~400 rows |
| f, OR, g | `f, e, d, g` | 1 | the OR's two columns are each fetched for 301 rows, g for the few left |
| **f, g, OR** | **`f, g, e, d`** (or `d, e`) | **1** | g cuts 301 rows to almost none, so the OR's columns are hardly fetched: chosen |
| g, f, OR / g, OR, f | `g, f, ...` | 1 | g is a wide text column; reading it in full costs more than reading f in full |
| OR, g, f | `(d, e), g, f` | 2 | as the first row, worse |

### What is declined, and why

- AO row-oriented tables: every column shares a varblock, nothing to skip.
- Quals with volatile functions or subplans.
- Quals with a `Param` (a generic plan's `$n`, or a nested loop's exec param):
  the stage quals run in a standalone `ExprContext` with no parameter values.
  Both `cost_aocs_stagedscan()` and `create_seqscan_plan()` (via
  `param_info`) and the executor's `stagedscan_applicable()` refuse them.
- At run time, a single-stage predicate whose first batch of survivors turns
  out to keep more than the selectivity gate, or to sit in nearly every block
  (fault injector `StagedScan_PhaseA_DenseFallback`), and a multi-stage
  predicate whose Phase A keeps several times what the planner estimated
  (`StagedScan_PhaseA_EstimateFallback`): Phase A hands the segment to the
  ordinary scan before emitting a row.
- System columns or whole-row references in the qual.
- Row-level security with a clause that could leak: `order_qual_clauses()`
  keeps a qual behind every qual of a lower security level unless it is
  leakproof, so a user's function never sees a row the policy hides. Rule 2
  of [What a Staged Scan guarantees](#what-a-staged-scan-guarantees) already
  keeps such a clause behind the policy's; on top of that, when the
  predicate mixes security levels and one of the guarded clauses is not
  leakproof the planner declines outright (`secret = 4242` with the
  leakproof `int4eq` still qualifies; `pad LIKE 'a%'` does not). Under GPORCA
  the plan no longer tells which clauses the policy contributed, so any
  query under row-level security keeps ORCA's qual order.
- An order that breaks rule 2 (a clause that can fail decided ahead of one
  the qual evaluates before it), from whichever optimizer.
- Under GPORCA, a scan whose range table entry is not a plain or partitioned
  table, and a partitioned table whose parent is not itself declared AOCO
  (the parent is what is decided, see below).

Not declined: a predicate and target list on the same single column, or no
target list at all (`SELECT count(*) FROM t WHERE d = X`). Nothing is left for
Phase B to fetch, and the cost model prices Phase A the same as the ordinary
scan's read of that column, so the two tie; the tie goes to the Staged Scan
because the model charges neither for forming tuples, which the ordinary scan
does for every row and the Staged Scan only for the survivors. On a 30M-row
table the fast path made `count(*)` on an integer predicate 4.5x faster, a
text `LIKE` ran a third faster through Phase A's row-at-a-time path, and a
predicate dense enough to fall back after the first batch was no slower than
the ordinary scan.

One limit comes from outside the scan. An AOCO column rewritten in place by
`ALTER COLUMN ... TYPE` or `SET ENCODING`, on a table whose segment files were
written by more than one insert session, can come out with row numbers that
disagree with the other columns' (a bug in the column-only rewrite). The
ordinary scan reads every column in lockstep and takes the row number from
the first projected column, so it never notices. A Staged Scan reads the
filter column and the fetched columns independently and is the first reader
to compare them. Depending on where the shifted rows fall, the scan stops
with an error (a stage cursor asked for a row its file has already passed, or
a column file ending before a survivor's row) or, with the rewritten column
fetched in Phase B, pairs a survivor with another row's value and nothing
reports it. Until the rewrite is fixed, queries over such a column should run
with `whpg_enable_staged_scan` off.

### GPORCA

ORCA builds its plans without a `PlannerInfo`, so the decision cannot run
inside it. It runs instead as a post-processing pass over ORCA's finished
plan (`stagedscan_decide_orca_plan()` in `stagedscancost.c`, called from
`optimize_query()`): every `SeqScan` and `DynamicSeqScan` over an AOCO
relation gets a scratch `RelOptInfo` built from the final range table through
`build_simple_rel()` -- the same catalog statistics the Postgres planner sees
-- with the scan's quals as its restrictions (ORCA's single conjunction split
back into clauses), its target list as the projection, and the survivors
estimated from the same statistics; `cost_aocs_stagedscan()` then decides
exactly as it does in `create_seqscan_plan()`. The scan splits ORCA's
conjunction into its clauses at run time too (`stagedscan_flatten_qual()`),
where the stages are assigned; the plan's qual is left as ORCA built it. A
scan the planner machinery could not cost without erroring (a qual Var of
another relation) is left alone rather than the query failed. Staged Scan is a refinement of
how a chosen table scan executes, not an alternative in the search space, so
ORCA's choice between a table scan and an index scan, and its join order, are
untouched; EXPLAIN shows `Staged Seq Scan ... Optimizer: GPORCA`.
`whpg_enable_staged_scan` governs this pass as it does the Postgres planner's;
there is no ORCA-only switch.

A `DynamicSeqScan` is decided once, on the partitioned parent, from the
inherited statistics (widths, selectivities) and the sum of the leaf
partitions' row counts. The inherited statistics carry no correlation, and
the cross-partition figure would be the wrong one anyway: the scan reads one
partition after another and skips blocks within each, so a column's
correlation is taken from the leaves' own statistics, weighted by their rows
and matched by column name (`staged_partition_correlation()`). The
executor applies the one stage order to every partition it opens, remapping
the stage attribute numbers per partition together with the quals
(`nodeDynamicSeqscan.c`), so a partition with its columns in another order is
fine. A clause partition pruning settles -- refuted or implied by the bound of
every partition, as an equality on the key of a list partition holding that
one value is -- keeps every row of each partition the scan opens, however
selective the parent's statistics make it look; it never stands as Phase A
alone (`staged_mark_pruned_clauses()`), and as a later stage it rejects
nothing and the run-time reordering moves it back. An equality on a range
partition's key, or on the key of a list partition holding several values,
does filter inside its partition and is costed as usual -- on the parent's
statistics, which can make it look more selective within the partition than
it is. (The Postgres
planner decides per partition, on the partition's own statistics, where such
a clause already estimates at 1.) The parent must itself be declared AOCO for the scan to qualify; a heap
partition under an AOCO parent simply ignores the request, since only the
AOCO access method implements `scan_begin_extractcolumns`, and keeps its
qual (`ExecInitSeqScan` drops it only for an AOCO relation). EXPLAIN shows
`Dynamic Staged Seq Scan`.

### Why no block-directory gate

An earlier version only ran on tables without a block directory, on the
theory that a table with one has an index and the planner should pick the
index path. The block directory is created when an index is built on the AO
table, or when the table asks for one with the `blockdirectory` storage
option. A gate on the directory would therefore exclude every indexed table
and every table that opted in. There is no such gate, and an index on
another column leaves the seq scan alone; what the directory changes is only
how Phase B moves between blocks, see below.

### Seeking through the block directory

The directory, when the table has one, is used instead. When the next survivor lies
far enough ahead that the walk would stream at least two read-ahead windows
of skipped content (estimated by `stagedscan_jump_is_long()` from the rows to
cross and the current block's shape, without touching the directory),
`stagedscan_seek_column()`
asks `AppendOnlyBlockDirectory_GetEntry()` for the varblock holding the row
and repositions the column's stream there with
`datumstreamread_block_seek()` (`AppendOnlyStorageRead_SetTemporaryStart()`
underneath: a fresh positioned read, no end limit, so the stream keeps
flowing sequentially afterwards). The blocks in between are neither read nor
stepped over. The lookup is cheap for ascending row numbers because the
directory caches the current minipage and entry. Short jumps are left to
the header walk: the next header or two sit in the bytes already buffered,
whereas a seek throws that buffer away for a fresh positioned read and then
another to resume. On the SF100 shipdate-day query, whose 249 371 hits touch
475 587 blocks, seeking on every jump meant 154 137 seeks and 9.3 s against
6.8 s for the plain walk; with the distance test it makes 351 seeks and runs
in 6.6 s, while the suppkey point lookup keeps its 2 328 long seeks
(1.1 s against 4.2 s walked).

The directory is advisory. Its entries are flushed a minipage at a time, so
the tail of a fresh insert has none, and a crashed insert can leave entries
past the file's EOF. A miss, an entry at or before the current block, or an
offset at or beyond the segfile's EOF are caught before any seek and fall
back to the header walk, which is always correct. Once the stream has been
repositioned there is no way back: an entry that points inside the file but
not at a block header is a corrupt directory and raises an error rather than
silently walking on. After a seek the block's `firstRowNum` is taken from the
header, or from the directory entry for pre-4.0 blocks that carry none.
A directory the scan's snapshot does not see is not opened at all: the
transaction that created it filled it in for the rows already there, so an
older snapshot sees none of those entries, and
`AppendOnlyBlockDirectory_Init_forSearch()` refuses such a snapshot rather
than report every row as missing. Phase B then walks headers as it does on a
table without a directory.

There is no switch for the seek: Phase B seeks whenever the table has a block
directory (that is, an index) and walks headers otherwise. The benchmarks
measure every shape on the table before and after it is given an index on an
unrelated column, which is exactly that difference.

What the seek saves on a cold cache is now measured (walk/seek table under
the SF100 section): evicting the tables' files with `posix_fadvise(DONTNEED)`
does make the next run read from the block device (the data directory is its
own device in the container, and the cgroup's I/O counters show it), and on
the scattered point lookup the seek reads 1.83 GB where the walk and the
ordinary scan read 10.45 GB, 2.8x faster than the walk cold. The cost model
still charges Phase B's I/O as `rho * R(c)` (the header walk's) rather than
`H(c)` random block reads, and it does not know that short jumps are better
walked than seeked (the same table's dense cases); both are follow-ups. The
charge is conservative in the right direction and the selectivity gate
decides most cases anyway.

### Phase A on the raw datum array

Phase A's row-at-a-time loop -- `datumstreamread_advance()`, then `ExecQual()`
on the value -- was most of its time once the decompression was paid: on the
SF100 order-key band 3.7 s of which zstd was well under a second. When Phase
A is a single column of type int2, int4, int8, date, timestamp or timestamptz
and every clause on it is `<column> <op> <constant>` or `<constant> <op>
<column>` with a btree comparison operator of the column's operator family
(any integer width for the integer family, exactly the column's type for the
datetime family, so that the comparison is a plain 64-bit integer compare),
`stagedscan_compile_simple()` turns the clauses into (strategy, constant)
pairs once, and each varblock that lays its datums out contiguously
(Original format, no NULL bitmap) is tested straight off its datum array by
`stagedscan_phase_a_scan_block()`; the stream is then positioned on the last
row consumed with `datumstreamread_position_fixedwidth()`, so a batch that
fills inside a block resumes where a row-at-a-time scan would have. Blocks
that do not qualify -- a NULL in the block, the RLE/delta (Dense) format --
and predicates that do not (`<>`, `IN`, an OR, a function, a cross-type
constant such as `date_col = timestamp '...'`, a pass-by-reference or
multi-column Phase A) take the row-at-a-time path, block by block, so a
column with a few NULLs still runs mostly fast. The two paths keep the same
counters and visimap check, and the fault point `StagedScan_PhaseA_FastBlock`
fires once per block tested this way (`staged_scan_fastpath`). Order-key
band 3.68 s -> 1.40 s, suppkey point 7.24 -> 4.32 s at SF100.

### EXPLAIN ANALYZE and "Rows Removed by Filter"

Rows that Phase A or a later stage drops never reach `ExecScan`, so the
executor's own `nfiltered1` would not see them. The scan reports them
(`TableScanInstrumentation.staged_rows_removed`: Phase A's rejections, not
counting deleted rows, plus every later stage's) and `nodeSeqscan.c` adds
them to `nfiltered1`, so `Rows Removed by Filter` under a staged scan
means what it means under an ordinary scan: rows the predicate removed,
whichever phase did the removing. `Stage Survivors: N of M rows` sits
next to it and says how far Phase A alone got; when the executor handed a
segment file back to the ordinary scan (missing values in a Phase A column,
or a first batch that showed the predicate too unselective or too dense) or
declined the two-phase scan when it opened the table,
`Stage Fallback: K segment files scanned without it` follows, counting
every segment file the ordinary scan read, and the survivors line covers
only the files that ran Phase A. `Stage Blocks:
D decompressed, S skipped` is Phase B's side of the bill: over every column
it fetched, how many blocks it decompressed because they held a survivor
and how many it passed over without decompressing them. A header walk
passes a block by reading its header; a seek through the block directory
passes a run of blocks without reading anything, so those are counted from
the directory's entries (`AppendOnlyBlockDirectory_CountEntriesBetween()`,
one heap tuple per 161 blocks crossed, only under EXPLAIN ANALYZE) and the
two paths bill the same blocks: a clustered band reads `26967 decompressed,
704483 skipped` on the table with a directory and on the copy without one.
(A directory built with `gp_blockdirectory_entry_min_range > 0` has entries
spanning several blocks; there the seeking path's skipped count is a lower
bound on the blocks jumped, since nothing reads their headers.)
The seek count itself is exposed in the structured EXPLAIN formats
(`Stage Blocks Seeked`). Phase A's blocks
are not in it: the filter column is read in full, as the ordinary scan
reads every column. The counters live in `StagedScanState` and are
published after every fetched row (`aocs_stagedscan_getnext()`), where
`nodeSeqscan.c` picks up the increment like the row counts. One difference from
the ordinary scan remains: Phase A reads a batch of survivors before Phase B
emits the first row, so under a `LIMIT` that stops early the count includes
the rows of the batches read so far, where the ordinary scan stops counting
at the row that satisfied the limit; with batches starting at 8 survivors the
difference is small. The partition scans of a Dynamic Seq Scan
carry no instrumentation of their own and add their counts to the dynamic
node's (`SeqScanState.staged_instr`). A heap partition under an AOCO
parent adds nothing there: a Dynamic Seq Scan reports no filtered rows for
its partitions in the first place, so on such a table `Rows Removed by
Filter` counts the AOCO partitions' rows only. (An earlier version reported
only the rows the ordinary filter removed, normally none; the tests that
read the count under AOCO, `uao_dml/ao_covering_index`, were switched to the
ordinary scan then and stay that way.)

### Binary compatibility

This tree is a released line, and extensions built against an earlier
release must keep working, so the feature changes no existing layout:

- The plan fields are on `SeqScan`, which has its own struct with `Scan` as
  its first member; `Scan`, and with it every other scan node (`IndexScan`,
  `ForeignScan`, `CustomScan`, ...), keeps its size and field offsets.
- The planner's decision reaches the access method as three arguments after
  `flags` of `scan_begin_extractcolumns`, read only when `flags` has
  `SO_STAGED_SCAN`, which only `table_beginscan_es_staged()` sets. An access
  method built before they existed finds every other argument where it
  expects it, and the AOCO method ignores them when called through an older
  `table_beginscan_es()`.
- The scan's counters hang off `AOCSScanDescData` (`stagedInstrument`), not
  off `TableScanDescData`, and `nodeSeqscan.c` reads them only for an AOCO
  relation under a Staged Scan plan.
- The new fields of `Instrumentation` and `AOStorageInfo` come after the
  existing ones.

## GUCs

| GUC | default | meaning |
|---|---|---|
| `whpg_enable_staged_scan` | on | master switch, for both optimizers |
| `whpg_staged_scan_min_selectivity` | 0.10 | the planner declines above this estimated selectivity of the whole predicate; the executor's first-batch re-checks use the same gate |

The second is a developer setting (category "Developer Options", hidden from
`SHOW ALL`): it is the one knob worth sweeping when the break-even point is
measured on a new workload, and a DBA has no reason to touch it. Both are
synchronized to the segments, where the scan runs.

Everything else that shapes the scan is a constant in `aocs_stagedscan.c`:
the survivor batch (`STAGEDSCAN_BATCH_FIRST_ROWS` 8, doubling up to
`STAGEDSCAN_BATCH_MAX_ROWS` 65536), the first-batch evidence
(`STAGED_DENSITY_MIN_BLOCKS` 64, `STAGED_DENSITY_FALLBACK_RATIO` 0.9,
`STAGED_ESTIMATE_SLACK` 4, `STAGED_ESTIMATE_MIN_GATE_MULT` 2) and the
reordering (`STAGED_ADAPT_INITIAL_WINDOW` 4096, `STAGED_ADAPT_MIN_STAGE_ROWS`
1000, `STAGED_ADAPT_HYSTERESIS` 1.25, `STAGED_ADAPT_SETTLE_CREDIT` 0.25,
`STAGED_ADAPT_BLOCK_ROWS` 16). The OR bypass and the reordering are always
on.

EXPLAIN shows a Seq Scan the planner turned into a staged scan as
`Staged Seq Scan on <table>` (the plan node is still a SeqScan; only the
name changes), with one line under it: `Stages: <Phase A columns>,
<stage 2>, ...` -- the columns in the order they are fetched, the Phase A
group parenthesized when a clause needs several columns, a single name for
a single-column predicate. The
predicate itself stays on the `Filter:` line and `Rows Removed by Filter`
counts what the predicate removed in any phase. EXPLAIN ANALYZE adds `Stage Survivors: <kept> of <scanned> rows`: how many rows passed Phase A (and
were visible) out of the rows Phase A read, summed over the segments
(carried through `TableScanInstrumentation` and GP's per-node stats, see
`explain_gp.c`); the rows read include deleted rows not yet vacuumed, and
only the partitions actually scanned. When a segment file was handed back to
the ordinary scan, `Stage Fallback: K segment files scanned without
it` follows. The last line, `Stage Blocks: D decompressed, S skipped`,
is Phase B's bill over the columns it fetched (the section on EXPLAIN
ANALYZE above says what is and is not counted). The headline query of the
SF100 section, on the copy without a block directory:

```
->  Staged Seq Scan on lineitem_walk (actual rows=10811.00 loops=1)
      Filter: ((l_orderkey >= 6000000) AND (l_orderkey <= 6032000))
      Rows Removed by Filter: 200032113
      Stages: l_orderkey
      Stage Survivors: 31695 of 600037902 rows
      Stage Blocks: 266 decompressed, 44828 skipped
```

Read it as: Phase A kept 31,695 of 600M rows; for the nine other columns
Phase B decompressed 266 blocks and passed over 44,828 without decompressing
them, which an ordinary scan would all have decompressed. (The figures here
and in the measurements below were taken while Phase B still walked the
Phase A column a second time to satisfy the recheck; since `l_orderkey` is
carried with the survivors, the line no longer includes that column's
blocks and reads a little lower.) The same line on
`lineitem`, which has a block directory, is identical: the seek changes how
the skipped blocks are passed, not which. A dense predicate reads the other
way round (one ship date: `519633 decompressed, 53232 skipped`), and a full
fallback prints no Blocks line at all.

## Tests and fault injectors

The regress tests observe the executor through fault injectors scoped to the
table name: `StagedScan_PhaseA_Survivor`, `StagedScan_PhaseA_Complete`,
`StagedScan_PhaseA_DenseFallback` (the first batch's survivors sat in nearly
every block and the segment went to the ordinary scan),
`StagedScan_PhaseA_EstimateFallback` (a multi-stage Phase A kept several
times the planner's estimate and the segment went to the ordinary scan),
`StagedScan_PhaseA_FastBlock` (a block Phase A tested on its raw datum
array), `StagedScan_PhaseA_Batch`, `StagedScan_PhaseA_BatchComplete` (not a
counter: armed with `skip`, it closes a batch on every survivor, which is how
the tests reach the batch boundaries a small table never fills by itself),
`StagedScan_PhaseB_BlockSkipped`, `StagedScan_PhaseB_Seek`,
`StagedScan_PhaseB_BlockDecompressed`, `StagedScan_Stage_BlockSkipped`,
`StagedScan_Stage_Seek`, `StagedScan_Stage_BlockDecompressed`,
`StagedScan_Stage_Rejected`, `StagedScan_Stage_Bypassed` (a disjunct settled
an OR clause before its last disjunct), `StagedScan_Stage_Reordered` (the
adaptive pass changed the order at a segfile boundary), plus
`AppendOnlyStorageRead_ReadBlockHeader` and
`AppendOnlyStorageRead_DecompressBlockContent` on the storage primitive.
Counters live in segment shared memory and are per name; arming an already
armed fault fails and keeps the old counter running. Read one with

```sql
substring(gp_inject_fault(f, 'status', dbid) from 'num times hit:''(\d+)''')::bigint
```

Measure on a table that spans several varblocks per segment: on a small table
every survivor lands in the first block, which `open_all_datumstreamread_segfiles()`
already read at scan open, so Phase B reports zero skipped and zero
decompressed while working perfectly. `StagedScan_PhaseA_Complete` tells you
Phase A ran at all.

## Measurements

Every number below comes from one 3-primary cluster on a single machine. The
SF100 section is on a release build (`-O2`, no assertions) and is the
reference; the earlier sections were timed on the development build (`-O0`,
assertions on), whose ratios are optimistic for scattered predicates, see the
SF100 section for why. They were taken while the feature was developed on a
newer PostgreSQL base; this tree runs the same storage and executor code
under the PostgreSQL 12 planner and executor and has not been re-benchmarked
at these sizes.

### WarehousePG

1M rows, 3 primary segments on one machine, warm cache, min of 5, from
`bench.sql`. `walk` is Staged Scan on the table without an index (no block
directory: Phase B walks headers), `seek` the same table after an index on an
unrelated column (Phase B seeks through the block directory), `off` the
ordinary scan. `gain` is seek against off.

| compress | distribution | sel | seek | walk | off | gain |
|---|---|---|---|---|---|---|
| none | clustered | 0.01% | 13.7 ms | 19.0 ms | 56.1 ms | 76% |
| none | scattered | 0.01% | 15.6 ms | 25.3 ms | 54.7 ms | 71% |
| none | clustered | 1% | 14.3 ms | 19.5 ms | 56.3 ms | 75% |
| none | scattered | 1% | 36.7 ms | 37.3 ms | 55.0 ms | 33% |
| none | clustered | 5% | 17.7 ms | 23.9 ms | 57.9 ms | 69% |
| none | scattered | 5% | 40.4 ms | 40.3 ms | 56.2 ms | 28% |
| zstd | clustered | 0.01% | 14.1 ms | 17.2 ms | 70.1 ms | 80% |
| zstd | scattered | 0.01% | 19.5 ms | 23.2 ms | 76.9 ms | 75% |
| zstd | clustered | 1% | 16.1 ms | 17.8 ms | 70.3 ms | 77% |
| zstd | scattered | 1% | 51.1 ms | 50.3 ms | 68.7 ms | 26% |
| zstd | clustered | 5% | 19.6 ms | 21.8 ms | 71.5 ms | 73% |
| zstd | scattered | 5% | 54.8 ms | 53.8 ms | 69.8 ms | 21% |

Where the survivors leave gaps of more than one block the seek beats the
header walk by 10-38% even with everything in the page cache (fewer headers
parsed, no content memcpy'd through the buffered reader; the uncompressed
table gains most, since header parsing is a larger share of its scan); where
they do not (scattered 1% and 5%: a survivor in nearly every block) no seek
is taken and the two are the same scan. Multi-column staging, measured on the
indexed tables: 47-48% on three scattered 10% columns, 67% on a scattered
column plus a two-column clause, 70-73% on a clustered band times a scattered
column; staging beat single-column-plus-recheck on every shape. All 61 on/off
pairs returned identical results.

### Realistic shapes (`bench_suite.sql`)

TPC-H-shaped `lineitem` at 6M rows (zstd, 277 MB), same cluster and method,
seek (indexed table) against the ordinary scan: a clustered `l_orderkey` band
433 -> 77 ms (82%), a `l_partkey` point lookup 418 -> 77 ms (82%), one
`l_shipdate` day 416 -> 168 ms (60%), a week 37%, a month 29%, a two-column
`l_shipmode AND l_shipdate` 58%, a three-column predicate 76%, a `LIKE`
prefix on `l_comment` 45%. Projecting all 16 columns the same band goes
1066 -> 84 ms (92%). On a 40-column table the ordinary scan grows linearly
with the projected width (150 / 497 / 963 ms for 5 / 20 / 40 columns) while
Staged Scan stays at 29 / 32 / 36 ms; the seek matters most there (header
walk 67 ms at 40 columns, the largest seek-over-walk gap, 46%). Gains are the
same at 1M and 6M rows, and zlib -- the most expensive codec to decompress --
gains most (87%). The two predicates above the selectivity gate (14%, 20%)
are declined and run the same plan as before. Without an index (the `walk`
column) every gain is 2-14 points lower and the ranking is unchanged. All 41
on/off pairs returned identical results.

Clauses that widen Phase A, same table: `l_partkey = X OR l_suppkey = Y`
(630 rows, Phase A reads both int columns) 483 -> 181 ms (63%), the same OR
projecting all 16 columns 1081 -> 285 ms (74%), `l_shipdate = D OR
l_partkey = X` 476 -> 233 ms (51%), a three-column OR 556 -> 252 ms (55%).
With a selective AND partner the planner keeps a one-column Phase A and
decides the OR at a later stage: `(l_partkey = X OR l_suppkey = Y) AND
l_shipdate BETWEEN ...` leads with `l_shipdate`, 533 -> 116 ms (78%). An OR
over 28% of the rows is declined and unchanged.

### OR bypass (`bench_suite.sql` group `bypass`)

Same 6M-row `li_zstd` (indexed, so seeking), one `l_shipdate` week (16,426
survivors) leading and an OR decided in Phase B; `bypass off` is the same
staging with the OR clause evaluated as one unit (a build with the bypass
disabled), `off` the ordinary scan, min of 5 warm runs. What the bypass buys is the fetch of the later disjunct's column,
and that is only worth something when the rows it is still fetched for are
sparse enough to leave whole varblocks untouched:

| predicate after the week | off | bypass off | bypass on | vs bypass off | stage blocks decompressed |
|---|---|---|---|---|---|
| `l_quantity > 1 OR l_comment LIKE '%zzz%'` (first disjunct true on 98%) | 570 ms | 328 ms | 231 ms | -30% | 9 922 -> 1 243 |
| `l_quantity > 1 OR l_comment LIKE ... OR l_suppkey = 777` (three disjuncts) | 607 ms | 350 ms | 238 ms | -32% | 10 654 -> 1 526 |
| `l_returnflag <> 'R' OR l_comment LIKE '%zzz%'` (true on 67%) | 571 ms | 326 ms | 281 ms | -14% | 9 372 -> 5 299 |
| `l_returnflag = 'N' OR l_partkey = X` (33%, narrow column) | 451 ms | 235 ms | 235 ms | 0% | 1 097 -> 1 096 |
| same as the first row but `l_comment` projected | 436 ms | 269 ms | 269 ms | 0% | 9 922 -> 9 922 |
| clustered `l_orderkey` band instead of the week, first row's OR | 560 ms | 98 ms | 98 ms | 0% | 177 -> 177 |

In the first row 328 of the 16,426 rows still need `l_comment`, so most of
its ~3,300 varblocks per segment are never decompressed. At 67% true, 5,475
rows still need it, one in nearly every block: half the blocks are still
decompressed and the time moves much less. A narrow int column has a survivor
in every block anyway; a projected column is always fetched (the bypass then
saves only the evaluation); and inside a contiguous band every block of the
band holds rows that still need the column, so there is nothing to skip. The
planner's fetch fraction models the sparse-rows effect and orders the
disjuncts accordingly (`l_quantity` before `l_comment`), but does not itself
decide whether the sparse rows fall into fewer blocks; the block-count formula
does. All 41 suite cases returned identical on/off results.

### Adaptive reordering at SF100 (`bench_adaptive_sf100.sql`)

Release build, `lineitem` SF100 (next section). `l_partkey BETWEEN 1000000
AND 1002000 AND l_comment <> 'zzz' AND l_suppkey <= 500`: the partkey range
keeps 0.01% (60,231 rows, well under one per block), `l_comment` is a
27-byte text column whose clause is always true, `l_suppkey <= 500` keeps 1
row in 2,000. Planted statistics (l_comment "keeps 5%", l_suppkey "keeps
99%") make the planner order `l_partkey, l_comment, l_suppkey`; the true
order is `l_partkey, l_suppkey, l_comment`.

| stage order | how | stage blocks decompressed | reorders | time |
|---|---|---|---|---|
| ordinary scan | Staged Scan off | -- | -- | 13.1-17.5 s |
| `l_partkey, l_comment, l_suppkey` | misled, adaptivity off | 107,363 | 0 | 5.6-7.0 s |
| same, re-scored at the segfile end only | misled, first version | 97,903 | 3 | 4.9-5.7 s |
| `..., l_comment, l_suppkey` -> `..., l_suppkey, l_comment` | misled, windows from 4,096 survivors | 55,725 | 3 | 4.3-5.1 s |
| `l_partkey, l_suppkey, l_comment` | true statistics | -- | -- | 4.3-5.9 s |

The block counts are the evidence (the timings sit at the edge of this
machine's noise, Phase A on `l_partkey` alone being over 3 s of each). Phase
A has to be sparse for the demonstration: with several survivors per block
every later stage costs a full read whatever its order, and the recalibrated
model then puts the wide column into Phase A or declines outright.

### TPC-H SF100 on the optimized build (`bench_scale.sql`)

Everything above is synthetic, at most 6M rows, and was timed on the
development build (`-O0`, assertions on). This section is the reference
measurement: real TPC-H SF100 (`gen_tpch.sh` runs dbgen chunked in parallel,
`load_tpch.sh` loads through gpfdist external tables, one instance per
segment), `lineitem` 600,037,902 rows in 16 columns, 18 GB AOCO zstd-1,
distributed by `l_orderkey`, one segfile per column and segment, on a
release build (`-O2`, no assertions). Two passes over two copies of the same
data: `walk` = Staged Scan on `lineitem_walk`, a `CREATE TABLE AS` copy that
has never had an index (no block directory, Phase B walks the block headers);
`seek` = Staged Scan on `lineitem`, which kept the block directory of an
earlier index build (Phase B seeks); `off` = the ordinary scan, timed in the
same pass beside its own Staged Scan runs, because the machine drifts
between passes (compare within a pass, never across). Min of 2 timed runs
after a warm-up; every timed run records the MB read from disk. Three
`off (directory)` controls had read from disk (a leftover of the cold runs
below) and were re-timed warm; the 16-column controls are the exception: a
16-column scan touches all 18 GB and with two copies present cannot stay in
the page cache, so they read 1.8-9.3 GB per run (in the earlier run, with one
copy, the same controls ran warm at 33.3 s and 32.3 s: about 5% inflation).
`blk_seek` / `blk_read` are Phase B's seeks and decompressed blocks over all
segments; `blk_read` is identical in both passes, `blk_seek` exists only in
the directory pass.

History: the first version of this section measured both
passes on `lineitem` alone, and its "no index" pass unknowingly seeked too,
because dropping an index does not drop the block directory. The two tables
below are the re-run on the two copies; the earlier run is still the source
of the staging and bypass comparisons quoted after the tables.

Header walk against block-directory seek on identical 18 GB copies
(`lineitem_walk`: no directory; `lineitem`: directory), same pass, ordinary
scan on the copy as control. Cold = the tables' files evicted from the page
cache before each run (`posix_fadvise(DONTNEED)`; the data directory is its
own block device in the container, `read` is what it served during the
statement); warm = best of two after a warm-up, 0 bytes read in every Staged
Scan run.

| shape | off cold | read | walk cold | read | seek cold | read | walk warm | seek warm |
|---|---|---|---|---|---|---|---|---|
| `l_orderkey` band | 14.8 s | 8.9 GB | 3.5 s | 534 MB | 3.6 s | 275 MB | 3.27 s | 3.28 s |
| `l_suppkey` point | 14.7 s | 10.5 GB | 11.1 s | 10.45 GB | 3.9 s | 1.83 GB | 3.10 s | 2.87 s |
| `l_shipdate` one day | 13.8 s | 9.8 GB | 11.7 s | 9.8 GB | 13.7 s | 9.5 GB | 9.15 s | 7.89 s |
| `l_returnflag AND l_linestatus AND l_shipdate` | 17.1 s | 10.1 GB | 10.8 s | 10.1 GB | 16.6 s | 8.1 GB | 8.25 s | 7.55 s |
| `l_orderkey` band, all 16 columns | 33.0 s | 18.8 GB | 3.9 s | 837 MB | 3.9 s | 286 MB | 3.50 s | 3.39 s |

The walk saves decompression, not I/O: it streams the skipped blocks' bytes
through the sequential reader (the point lookup reads the same 10.45 GB as
the ordinary scan) and stops only at the last survivor, which is why the
bands read little. The seek turns skipped blocks into skipped I/O (point
lookup 1.83 GB, 2.8x faster than the walk cold) and is 7-14% ahead warm on
scattered predicates, level on clustered ones. Where a hit sits in nearly
every block the seek loses cold -- fewer bytes, but short positioned reads
defeat the kernel's read-ahead that the walk's sequential stream enjoys
(three-column predicate 16.6 s against 10.8 s). These rows were measured
seeking on every jump that was not the very next block; the shipped scan
seeks only for jumps long enough to leave the read-ahead window and walks the
dense cases (see "Seeking through the block directory" above).

| shape | matched | stages | off (no index) | walk | walk vs off | off (directory) | seek | seek vs off | blk_seek | blk_read |
|---|---|---|---|---|---|---|---|---|---|---|
| `orderkey band (clustered, 32k keys)` | 31,695 | l_orderkey | 14.0 s | 3.1 s | -78% | 14.7 s | 3.2 s | -78% | 12 | 154 |
| `partkey point` | 24 | l_partkey | 14.1 s | 3.6 s | -74% | 12.5 s | 2.8 s | -78% | 96 | 96 |
| `suppkey point` | 615 | l_suppkey | 12.1 s | 4.0 s | -67% | 14.3 s | 2.7 s | -81% | 2,441 | 2,454 |
| `shipdate day (scattered)` | 249,371 | l_shipdate | 12.9 s | 8.0 s | -38% | 16.9 s | 9.7 s | -42% | 154,137 | 475,587 |
| `shipdate week` | 1.7M | l_shipdate, fell back | 12.5 s | 13.2 s | +6% | 14.6 s | 14.1 s | -4% | 0 | 0 |
| `shipdate month` | 7.7M | l_shipdate, fell back | 12.3 s | 13.2 s | +8% | 17.2 s | 15.7 s | -9% | 0 | 0 |
| `shipmode AND shipdate 2 weeks (2 cols)` | 499,530 | l_shipdate, l_shipmode | 14.0 s | 11.9 s | -15% | 21.8 s | 15.9 s | -27% | 126,817 | 627,015 |
| `returnflag AND linestatus AND shipdate (3 cols)` | 124,752 | l_shipdate, l_returnflag, l_linestatus | 15.6 s | 7.7 s | -51% | 16.2 s | 10.2 s | -37% | 153,792 | 323,471 |
| `comment LIKE prefix` | 58,515 | l_comment | 11.5 s | 8.3 s | -28% | 16.0 s | 8.7 s | -46% | 106,022 | 147,631 |
| `orderkey band, all 16 columns` | 31,695 | l_orderkey | 34.5 s | 3.3 s | -90% | 40.7 s | 3.1 s | -92% | 48 | 401 |
| `shipdate day, all 16 columns` | 249,371 | l_shipdate | 33.4 s | 21.8 s | -35% | 34.4 s | 22.1 s | -36% | 355,615 | 1.5M |
| `quantity < 11 (20%, declined)` | 120.0M | declined | 16.3 s | 15.4 s | -6% | 17.7 s | 17.4 s | -2% | 0 | 0 |
| `partkey OR suppkey` | 639 | (l_partkey, l_suppkey) | 15.6 s | 5.7 s | -64% | 15.9 s | 4.8 s | -70% | 3,169 | 3,187 |
| `(partkey OR suppkey) AND shipdate week` | 2 | (l_partkey, l_suppkey), l_shipdate | 17.7 s | 5.1 s | -71% | 20.1 s | 5.2 s | -74% | 10 | 10 |
| `shipdate day OR partkey` | 249,395 | (l_partkey, l_shipdate) | 15.2 s | 11.6 s | -24% | 19.4 s | 11.9 s | -39% | 156,586 | 546,409 |
| `week AND (quantity > 1 [98%] OR comment LIKE)` | 1.7M | l_shipdate, l_quantity, l_comment | 16.9 s | 10.5 s | -38% | 16.5 s | 10.4 s | -37% | 103 | 521,841 |
| `week AND (returnflag <> R [67%] OR comment LIKE)` | 872,429 | l_shipdate, l_returnflag, l_comment | 16.3 s | 12.5 s | -23% | 16.3 s | 16.5 s | +1% | 4,178 | 517,692 |

Selectivity against clustering, same projection (4 columns):

| shape | matched | stages | off (no index) | walk | walk vs off | off (directory) | seek | seek vs off | blk_seek | blk_read |
|---|---|---|---|---|---|---|---|---|---|---|
| `clustered 0.01% (orderkey band)` | 59,919 | l_orderkey | 13.1 s | 3.6 s | -73% | 15.0 s | 3.1 s | -79% | 16 | 287 |
| `clustered 0.1%` | 599,704 | l_orderkey | 12.4 s | 3.7 s | -71% | 12.7 s | 3.2 s | -75% | 16 | 2,720 |
| `clustered 1%` | 6.0M | l_orderkey | 13.1 s | 3.9 s | -70% | 13.5 s | 3.5 s | -74% | 16 | 26,967 |
| `clustered 5%` | 30.0M | l_orderkey | 13.7 s | 5.2 s | -62% | 17.4 s | 5.0 s | -71% | 12 | 112,622 |
| `scattered 0.01% (partkey range)` | 60,231 | l_partkey | 12.2 s | 6.5 s | -47% | 15.8 s | 6.8 s | -57% | 125,496 | 192,072 |
| `scattered 0.1%` | 600,812 | l_partkey, fell back | 12.4 s | 13.2 s | +6% | 13.3 s | 13.6 s | +2% | 0 | 0 |
| `scattered 1%` | 6.0M | l_partkey, fell back | 13.9 s | 13.7 s | -1% | 14.0 s | 16.9 s | +21% | 0 | 0 |
| `scattered 5%` | 30.0M | l_partkey, fell back | 13.9 s | 13.6 s | -2% | 13.5 s | 14.4 s | +7% | 0 | 0 |
| `quantity < 2 (2%)` | 12.0M | l_quantity, fell back | 13.4 s | 13.0 s | -3% | 12.8 s | 14.0 s | +10% | 0 | 0 |
| `quantity < 4 (6%)` | 36.0M | l_quantity, fell back | 15.0 s | 14.4 s | -4% | 14.4 s | 15.9 s | +11% | 0 | 0 |
| `quantity < 5 (8%)` | 48.0M | l_quantity, fell back | 14.1 s | 15.8 s | +12% | 15.5 s | 14.7 s | -5% | 0 | 0 |
| `quantity < 6 (10%)` | 60.0M | declined | 18.9 s | 17.6 s | -7% | 14.0 s | 15.5 s | +11% | 0 | 0 |
| `quantity < 7 (12%)` | 72.0M | declined | 19.9 s | 20.6 s | +4% | 18.1 s | 15.9 s | -12% | 0 | 0 |
| `quantity < 8 (14%)` | 84.0M | declined | 17.5 s | 18.2 s | +4% | 19.4 s | 14.1 s | -27% | 0 | 0 |

Staging against a single-column Phase A with executor recheck (a forced
one-stage order that existed as a developer switch at the time; earlier
run on `lineitem`, all variants in one pass): two-column AND
13.4 s vs 19.8 s (-33%), three-column AND 8.6 vs 19.3 s (-55%), OR under
AND 5.4 vs 15.8 s (-65%). Bypass against the same stage order with the
bypass off: 11.1 vs 18.3 s (-39%) with a 98%-true first disjunct, 14.0 vs
15.4 s (-9%) with a 67%-true one. All 31 cases returned identical results
on and off on both tables (124 pairs), and the two EXPLAIN ANALYZE headline
queries below did too.

What the optimized build changed, and what was done about it. On the
development build the ordinary scan's own per-row work was large, so a
scattered predicate with several survivors per block still measured 30-50%
faster with Staged Scan (the 6M-row and SF30 tables above). At `-O2` the
ordinary scan is about 2.7x faster and Phase B only about 2x, and those
predicates came out level or slightly behind: Phase B positions inside a
hit block datum by datum (`datumstreamread_find()`), so with survivors in
nearly every block it does nearly all the unpacking the ordinary scan does,
plus a header walk the model never charged. The cost model now charges both
(the `T(c)·U` and `ρ·B(c)·W` terms in the section above, fitted on the
selectivity sweep), a single-stage predicate the pessimistic estimate
declines is re-costed as if clustered (the correlation statistic is blind to
parallel-loaded tables: `l_orderkey` here has correlation -0.09 and its 1%
band still gains 70%), and Phase A falls back to the ordinary scan on the
first batch when the survivors turn out dense. In the tables above the rows
marked "fell back" are that path: the plan says Staged Seq Scan, the
segment ran as an ordinary scan after one batch of Phase A, and the cost is
within the pass's noise (the same machine drifted up to 25% between passes
under host memory pressure, which is why each pass carries its own control).

Two headline queries, EXPLAIN ANALYZE, on and off, on `lineitem_walk` (no
index, no block directory). A 32,000-key order band with 10 of the 16
columns projected and TPC-H Q1-style aggregates: 25.9 s -> 3.1 s, `Stage Survivors: 31695 of 600037902 rows`, `Stage Blocks: 266
decompressed, 44828 skipped`, with the same `Rows Removed by Filter` as the
ordinary scan reports, since the count includes what Phase A rejected. One
ship date with the Q1 aggregates over four numeric columns: 13.1 s -> 10.3 s,
`Stage Blocks: 519633 decompressed, 53232 skipped`: nine blocks in ten
hold a hit, the scattered case at its best, where the only saving is not
unpacking the other 599.8M rows (with the block directory the same query ran
9.8 s against 13.6 s in the earlier run).

### Earlier measurements (development build)

The 1M/6M-row sections above were timed on the development build, as was
a debug-build SF30/SF100 run that preceded the section above and is not kept
here. They established the shapes, the stage orders and the on/off
correctness, but their absolute times and their gains on scattered
predicates do not carry over to the optimized build.

## How other column stores order predicates, and the references

Surveyed while deciding between column permutations, clause orders and a rank
formula. The short version: nobody permutes columns; everyone
orders the *predicates*, by a cost-and-selectivity rank at plan time, by size,
or adaptively; and the disjunction problem is handled either not at all or by
tracking, per row or per set, which clauses are still open.

| system | what orders the predicates | how it handles OR / shared columns |
|---|---|---|
| PostgreSQL | `order_qual_clauses()` sorts a scan's quals by estimated per-tuple cost only (cheapest first; a security-barrier level pins some). Selectivity is ignored. | An OR is one clause; nothing is settled early beyond the `BoolExpr` short-circuit inside `ExecQual`. |
| ClickHouse | Multi-step `PREWHERE` (23.2+, `move_all_conditions_to_prewhere`, `enable_multiple_prewhere_read_steps`): conditions are split by the columns they need and ordered by the *uncompressed size* of those columns, smallest first; each step reads its columns for the rows still alive and produces a filter for the next. | A condition needing several columns is one step; an OR is not split. Columns already read by an earlier step are not read again. |
| DuckDB | Plan time: filters pushed into the scan in the optimizer's order. Run time: `AdaptiveFilter` keeps a permutation of the filters and, every few vectors, swaps two adjacent filters at random, keeps the swap if the measured time per vector improved, and otherwise reverts; the swap probability decays as the order stabilizes. | Filters are conjunctive per column (`TableFilterSet`); an OR is not pushed into the scan and runs as a plain filter above it. |
| Apache Doris | "Predicate columns first": the columns any predicate references are read first for a batch, the predicates evaluated to a selection vector, and the remaining columns read through that vector (issue #2545; `VerticalBlockReader` / lazy materialization). Predicates ordered by estimated selectivity, with runtime filters ahead. | The OR-clauses are evaluated as a whole on the selection vector; the predicate columns are read in full for the batch either way. |
| C-Store lineage | Late materialization: Abadi et al. compare LM-parallel (all predicate columns read, positions intersected by bitmap) with LM-pipelined (one predicate column read first, the next read only at the surviving positions) and find pipelined wins when the first predicate is selective. Staged Scan is LM-pipelined with the "first predicate" being a whole clause order. | Positions are the unit; a disjunction is a union of position lists. |

References:

- F. Kastrati and G. Moerkotte, *Optimization of Conjunctive Predicates for
  Main Memory Column Stores*, PVLDB 9(12), 2016. The rank ordering
  (selectivity - 1) / cost is optimal only for independent predicates with
  constant cost; with correlated predicates or costs that depend on the
  predicates already applied the problem is NP-hard and needs search. The
  reason the cost model here costs each candidate end to end instead of
  ranking.
- K. Krishnamurthy, H. Boral and C. Zaniolo, *Optimization of Nonrecursive
  Queries*, VLDB 1986, and M. Hellerstein and M. Stonebraker, *Predicate
  Migration*, SIGMOD 1993, for the rank ordering itself, which the adaptive
  pass applies with measured costs.
- S. Babu, R. Motwani, K. Munagala, I. Nishizawa and J. Widom, *Adaptive
  Ordering of Pipelined Stream Filters*, SIGMOD 2004. A-Greedy: keep a profile
  of which filters rejected which tuples, reorder when the profile shows a
  violation of the greedy invariant beyond a threshold. The adaptive pass here
  is a one-swap-per-segfile version of it.
- D. Abadi, D. Myers, D. DeWitt and S. Madden, *Materialization Strategies in
  a Column-Oriented DBMS*, ICDE 2007. LM-parallel versus LM-pipelined, and
  when each wins.
- A. Kim, A. Ileri and S. Madden, *Optimizing Query Predicates with
  Disjunctions for Column-Oriented Engines* (EvalPred), arXiv 2002.00540.
  The set-management formulation of disjunctions: track the set of rows still
  undecided per clause instead of evaluating each disjunct over the whole
  input; optimal for nesting depth <= 2 where the earlier TDACB search is
  exponential.
- ClickHouse: `PREWHERE` documentation and PR #46140 (multi-step PREWHERE,
  release 23.2); `src/Storages/MergeTree/MergeTreeWhereOptimizer.cpp` for the
  size-based ordering.
- DuckDB: `src/execution/adaptive_filter.cpp` (`AdaptiveFilter::AdaptRuntimeStatistics`).
- Apache Doris: issue #2545, "Read predicate columns first, then other
  columns", and `be/src/olap/rowset/segment_v2/segment_iterator.cpp`.
