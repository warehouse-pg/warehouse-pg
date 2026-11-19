/*
 * Unit test of the runtime filter's Bloom filter (runtimefilter.c): sizing,
 * insert/contains, the drop of an overfull filter, merge, the Param guard,
 * the switch-off tally, the inline integer hash, the key estimate read off
 * the fill, the fold of an oversized filter and the restart of an overfull
 * one.
 *
 * The filter cannot be called from SQL, so its limits are tested here; what
 * a query sees of them is in src/test/regress/sql/runtime_filter.sql.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "utils/memutils.h"

#include "../runtimefilter.c"

extern Datum hashint2(PG_FUNCTION_ARGS);
extern Datum hashint4(PG_FUNCTION_ARGS);
extern Datum hashint8(PG_FUNCTION_ARGS);

/* fmgr.c is mocked (see the Makefile), so call a hash function by hand */
static uint32
call_hash(PGFunction fn, Datum arg)
{
	LOCAL_FCINFO(fcinfo, 1);

	InitFunctionCallInfoData(*fcinfo, NULL, 1, InvalidOid, NULL, NULL);
	fcinfo->args[0].value = arg;
	fcinfo->args[0].isnull = false;
	return DatumGetUInt32(fn(fcinfo));
}

#define NONMEMBER_BASE	0x40000000U

static void
reset_gucs(void)
{
	whpg_runtime_filter_max_size = 4096;
	whpg_runtime_filter_disable_ratio = 0.9;
}

/* the hash value a join would compute for the int4 key i */
static uint32
hv(uint32 i)
{
	return DatumGetUInt32(hash_uint32(i));
}

/* a builder sized for 'est' keys holding the keys first .. first + n - 1 */
static RuntimeFilterBuilder *
build(double est, uint32 first, uint32 n)
{
	RuntimeFilterBuilder *b = runtimefilter_create(est, TopMemoryContext);

	for (uint32 i = 0; i < n; i++)
		runtimefilter_insert(b, hv(first + i));
	return b;
}

static double
fill_of(RuntimeFilter *f)
{
	Size		nbits = ((Size) 1 << f->log2_blocks) * RF_BLOCK_BITS;

	return (double) pg_popcount((const char *) f->blocks, nbits / 8) / (double) nbits;
}

/* how many of n keys that were never inserted the filter lets through */
static uint32
false_positives(RuntimeFilter *f, uint32 n)
{
	uint32		fp = 0;

	for (uint32 i = 0; i < n; i++)
	{
		if (runtimefilter_contains(f, hv(NONMEMBER_BASE + i)))
			fp++;
	}
	return fp;
}

static uint32
log2_for(double est)
{
	RuntimeFilterBuilder *b = runtimefilter_create(est, TopMemoryContext);
	RuntimeFilter *f = b->filter;
	uint32		log2 = f->log2_blocks;

	assert_int_equal(f->magic, RF_MAGIC);
	assert_int_equal(VARSIZE(f), RF_SIZE(log2));
	assert_int_equal(f->nkeys, 0);
	assert_int_equal(f->seen, 0);
	assert_int_equal(f->passed, 0);
	assert_int_equal(f->dead, 0);
	assert_int_equal(f->rest_rows, 0);
	assert_int_equal(f->rest_windows, RF_REST_MIN_WINDOWS);
	assert_int_equal(f->window, RF_ADAPT_JUDGE_ROWS);
	assert_true(fill_of(f) == 0.0);
	pfree(runtimefilter_finish(b));
	return log2;
}

/* 16 bits per key, a power of two of 256-bit blocks, 8 kB at least, max_size at most */
static void
test__create__sizes_by_estimate_and_limit(void **state)
{
	reset_gucs();
	assert_int_equal(log2_for(0), 8);
	assert_int_equal(log2_for(1), 8);
	assert_int_equal(log2_for(4096), 8);	/* 65536 bits exactly */
	assert_int_equal(log2_for(4097), 9);
	assert_int_equal(log2_for(1e6), 16);
	assert_int_equal(log2_for(1e9), 17);	/* capped at 4 MB */

	whpg_runtime_filter_max_size = 64;
	assert_int_equal(log2_for(1e9), 11);
	whpg_runtime_filter_max_size = 1;		/* under the minimum: the minimum */
	assert_int_equal(log2_for(1e9), 8);
	whpg_runtime_filter_max_size = 64 * 1024;
	assert_int_equal(log2_for(1e12), 21);
}

/* the planner's gate: 6 bits per key within max_size */
static void
test__fits__six_bits_per_key(void **state)
{
	reset_gucs();
	assert_true(runtimefilter_fits(0));
	assert_true(runtimefilter_fits(5592405));
	assert_false(runtimefilter_fits(5592406));
	whpg_runtime_filter_max_size = 8;
	assert_true(runtimefilter_fits(10922));
	assert_false(runtimefilter_fits(10923));
}

/* every inserted key is found, at any fill */
static void
test__contains__no_false_negatives(void **state)
{
	static const uint32 ns[] = {1, 100, 5000, 11000, 100000};

	reset_gucs();
	for (int t = 0; t < lengthof(ns); t++)
	{
		for (int sized = 0; sized < 2; sized++)
		{
			/* sized for the keys, and sized for one key (the 8 kB minimum) */
			RuntimeFilterBuilder *b = build(sized ? ns[t] : 1, 0, ns[t]);
			RuntimeFilter *f = b->filter;

			for (uint32 i = 0; i < ns[t]; i++)
				assert_true(runtimefilter_contains(f, hv(i)));
			runtimefilter_finish(b);
		}
	}
}

/* sized for a million, filled with a thousand: all but empty, nothing else passes */
static void
test__sparse__million_sized_thousand_filled(void **state)
{
	RuntimeFilter *f;
	uint32		fp;

	reset_gucs();
	f = (RuntimeFilter *) runtimefilter_finish(build(1e6, 0, 1000));
	assert_true(f != NULL);
	assert_int_equal(f->log2_blocks, 16);
	assert_int_equal(f->nkeys, 1000);
	assert_true(fill_of(f) <= 8000.0 / (65536.0 * 256));
	fp = false_positives(f, 1000000);
	printf("sparse: fill %.6f, %u false positives in 1000000\n", fill_of(f), fp);
	assert_true(fp < 10);
	pfree(f);
}

/* sized for its keys: about a third full, under 1% false positives */
static void
test__sized__false_positive_rate(void **state)
{
	RuntimeFilter *f;
	uint32		fp;

	reset_gucs();
	f = (RuntimeFilter *) runtimefilter_finish(build(100000, 0, 100000));
	assert_true(f != NULL);
	assert_int_equal(f->log2_blocks, 13);
	assert_int_equal(f->nkeys, 100000);
	fp = false_positives(f, 1000000);
	printf("sized: fill %.4f, %u false positives in 1000000\n", fill_of(f), fp);
	assert_true(fill_of(f) > 0.25 && fill_of(f) < 0.40);
	assert_true(fp < 10000);
	pfree(f);
}

/*
 * Sized for a thousand, filled with far more: kept up to 80% of the bits
 * set, dropped beyond. In 256 blocks the expected fill is 1 - exp(-n/8192):
 * 49% at 5,590 keys, 74% at 11,000, 84% at 15,000.
 */
static void
test__finish__drops_overfull_filter(void **state)
{
	RuntimeFilter *f;

	reset_gucs();
	f = (RuntimeFilter *) runtimefilter_finish(build(1000, 0, 5590));
	assert_true(f != NULL);
	printf("half: fill %.4f, %u false positives in 1000000\n",
				  fill_of(f), false_positives(f, 1000000));
	assert_true(fill_of(f) > 0.45 && fill_of(f) < 0.53);
	pfree(f);

	f = (RuntimeFilter *) runtimefilter_finish(build(1000, 0, 11000));
	assert_true(f != NULL);
	printf("near full: fill %.4f, %u false positives in 1000000\n",
				  fill_of(f), false_positives(f, 1000000));
	assert_true(fill_of(f) > 0.70 && fill_of(f) <= RF_MAX_FILL);
	pfree(f);

	assert_true(runtimefilter_finish(build(1000, 0, 15000)) == NULL);
	assert_true(runtimefilter_finish(build(1000, 0, 1000000)) == NULL);
}

/* fullness is judged on bits set, not rows: many rows over few keys keep the filter */
static void
test__finish__duplicates_do_not_fill(void **state)
{
	RuntimeFilterBuilder *b;
	RuntimeFilter *f;

	reset_gucs();
	b = runtimefilter_create(1000, TopMemoryContext);
	for (uint32 i = 0; i < 1000000; i++)
		runtimefilter_insert(b, hv(i % 100));
	f = (RuntimeFilter *) runtimefilter_finish(b);
	assert_true(f != NULL);
	assert_int_equal(f->nkeys, 1000000);	/* rows inserted, not distinct keys */
	assert_true(fill_of(f) <= 800.0 / 65536);
	pfree(f);
}

static void
test__merge__union_of_both_key_sets(void **state)
{
	RuntimeFilter *a;
	RuntimeFilter *b;

	reset_gucs();
	a = (RuntimeFilter *) runtimefilter_finish(build(20000, 0, 10000));
	b = (RuntimeFilter *) runtimefilter_finish(build(20000, 500000, 10000));
	assert_true(a != NULL && b != NULL);

	/* the tally is the probing process's state, a merge leaves it alone */
	a->seen = 5;
	a->passed = 3;
	b->seen = 100;
	b->passed = 100;
	b->rest_rows = 4096;

	assert_true(runtimefilter_merge((struct varlena *) a, (struct varlena *) b));
	for (uint32 i = 0; i < 10000; i++)
	{
		assert_true(runtimefilter_contains(a, hv(i)));
		assert_true(runtimefilter_contains(a, hv(500000 + i)));
	}
	assert_int_equal(a->nkeys, 20000);
	assert_int_equal(a->seen, 5);
	assert_int_equal(a->passed, 3);
	assert_int_equal(a->rest_rows, 0);
	assert_true(false_positives(a, 1000000) < 10000);

	/* the key count saturates */
	a->nkeys = PG_UINT32_MAX - 1;
	assert_true(runtimefilter_merge((struct varlena *) a, (struct varlena *) b));
	assert_true(a->nkeys == PG_UINT32_MAX);
	pfree(a);
	pfree(b);
}

static void
test__merge__refuses_unlike_filters(void **state)
{
	RuntimeFilter *a;
	RuntimeFilter *b;
	RuntimeFilter *other;
	RuntimeFilter *before;
	Size		size;

	reset_gucs();
	a = (RuntimeFilter *) runtimefilter_finish(build(20000, 0, 10000));
	b = (RuntimeFilter *) runtimefilter_finish(build(20000, 500000, 10000));
	other = (RuntimeFilter *) runtimefilter_finish(build(100000, 500000, 10000));
	size = VARSIZE(a);
	before = (RuntimeFilter *) palloc(size);
	memcpy(before, a, size);

	/* another size */
	assert_false(runtimefilter_merge((struct varlena *) a, (struct varlena *) other));
	assert_int_equal(memcmp(before, a, size), 0);

	/* other flags */
	b->flags = 1;
	assert_false(runtimefilter_merge((struct varlena *) a, (struct varlena *) b));
	assert_int_equal(memcmp(before, a, size), 0);
	b->flags = 0;

	/* not a filter, on either side */
	b->magic = 0;
	assert_false(runtimefilter_merge((struct varlena *) a, (struct varlena *) b));
	assert_false(runtimefilter_merge((struct varlena *) b, (struct varlena *) a));
	assert_int_equal(memcmp(before, a, size), 0);

	pfree(a);
	pfree(b);
	pfree(other);
	pfree(before);
}

/* what a Param holds is a filter only if its magic, block count and size agree */
static void
test__check__rejects_what_is_not_a_filter(void **state)
{
	RuntimeFilter *f;
	struct varlena *tiny;
	Size		size;

	reset_gucs();
	f = (RuntimeFilter *) runtimefilter_finish(build(1000, 0, 1000));
	size = VARSIZE(f);
	assert_true(runtimefilter_check((struct varlena *) f) == (struct varlena *) f);

	f->magic = RF_MAGIC + 1;
	assert_true(runtimefilter_check((struct varlena *) f) == NULL);
	f->magic = RF_MAGIC;

	f->log2_blocks = RF_MAX_LOG2_BLOCKS + 1;
	assert_true(runtimefilter_check((struct varlena *) f) == NULL);
	f->log2_blocks = 9;			/* a valid count, but not this filter's size */
	assert_true(runtimefilter_check((struct varlena *) f) == NULL);
	f->log2_blocks = 8;

	SET_VARSIZE(f, size - sizeof(uint32));	/* truncated */
	assert_true(runtimefilter_check((struct varlena *) f) == NULL);
	SET_VARSIZE(f, size);
	assert_true(runtimefilter_check((struct varlena *) f) == (struct varlena *) f);

	/* shorter than a filter's header: rejected without reading past it */
	tiny = (struct varlena *) palloc0(8);
	SET_VARSIZE(tiny, 8);
	assert_true(runtimefilter_check(tiny) == NULL);

	pfree(tiny);
	pfree(f);
}

/* probe 'n' rows, 'pct' in a hundred of them passing */
static void
probe_rows(RuntimeFilter *f, int n, int pct)
{
	for (int i = 0; i < n; i++)
		runtimefilter_probe_count(f, (i % 100) < pct);
}

/*
 * The watch on the filter's usefulness: a filter at work is judged 65,536
 * rows at a time; a useless one rests and is sampled for 4,096 rows after
 * each rest, resting longer each time, and one whose sample removes rows
 * is back at work.
 */
static void
test__probe_count__rests_and_wakes(void **state)
{
	RuntimeFilter *f;

	reset_gucs();
	f = (RuntimeFilter *) runtimefilter_finish(build(1000, 0, 1000));

	/* every row passes: at rest at the 65,536th, not a row sooner */
	probe_rows(f, RF_ADAPT_JUDGE_ROWS - 1, 100);
	assert_false(runtimefilter_resting(f));
	runtimefilter_probe_count(f, true);
	assert_true(runtimefilter_resting(f));
	assert_true(f->rest_rows == (int64) RF_REST_MIN_WINDOWS * RF_ADAPT_WINDOW);
	assert_true(f->seen == 0 && f->passed == 0);

	/* the rest is counted off by whoever lets rows by, in any step */
	runtimefilter_rest(f, 1);
	runtimefilter_rest(f, RF_ADAPT_WINDOW);
	assert_true(runtimefilter_resting(f));
	runtimefilter_rest(f, (int64) RF_REST_MIN_WINDOWS * RF_ADAPT_WINDOW);
	assert_false(runtimefilter_resting(f));
	assert_int_equal(f->rest_rows, 0);

	/* the sample after a rest is one window; useless again, the rest doubles */
	probe_rows(f, RF_ADAPT_WINDOW - 1, 100);
	assert_false(runtimefilter_resting(f));
	runtimefilter_probe_count(f, true);
	assert_true(f->rest_rows == (int64) 2 * RF_REST_MIN_WINDOWS * RF_ADAPT_WINDOW);
	runtimefilter_rest(f, f->rest_rows);

	/* ... up to the longest rest, and no further */
	for (int i = 0; i < 6; i++)
	{
		probe_rows(f, RF_ADAPT_WINDOW, 100);
		assert_true(runtimefilter_resting(f));
		assert_true(f->rest_rows <= (int64) RF_REST_MAX_WINDOWS * RF_ADAPT_WINDOW);
		if (i == 5)
			assert_true(f->rest_rows == (int64) RF_REST_MAX_WINDOWS * RF_ADAPT_WINDOW);
		runtimefilter_rest(f, f->rest_rows);
	}

	/*
	 * The rows change under it -- the matching rows came first in the table
	 * -- and the sample shows the filter removing rows: it is back at work,
	 * judged 65,536 rows at a time, its next rest the shortest again.
	 */
	probe_rows(f, RF_ADAPT_WINDOW, 1);
	assert_false(runtimefilter_resting(f));
	assert_int_equal(f->rest_windows, RF_REST_MIN_WINDOWS);
	probe_rows(f, 2000000, 1);
	assert_false(runtimefilter_resting(f));
	probe_rows(f, RF_ADAPT_JUDGE_ROWS - (2000000 % RF_ADAPT_JUDGE_ROWS), 1);
	assert_true(f->seen == 0);
	probe_rows(f, RF_ADAPT_JUDGE_ROWS, 100);
	assert_true(f->rest_rows == (int64) RF_REST_MIN_WINDOWS * RF_ADAPT_WINDOW);
	runtimefilter_wake(f);
	probe_rows(f, RF_ADAPT_WINDOW, 1);

	/* a burst of matching rows inside the rows judged does not rest it */
	probe_rows(f, RF_ADAPT_JUDGE_ROWS / 2, 100);
	probe_rows(f, RF_ADAPT_JUDGE_ROWS / 2, 1);
	assert_false(runtimefilter_resting(f));

	/* 89% pass: never rested; 91%: rested at the first judgement */
	probe_rows(f, 2000000 - 2000000 % RF_ADAPT_JUDGE_ROWS, 89);
	assert_false(runtimefilter_resting(f));
	probe_rows(f, RF_ADAPT_JUDGE_ROWS, 91);
	assert_true(runtimefilter_resting(f));
	runtimefilter_wake(f);

	/* a ratio of 1 never rests the filter */
	whpg_runtime_filter_disable_ratio = 1.0;
	probe_rows(f, 1000000, 100);
	assert_false(runtimefilter_resting(f));
	pfree(f);

	/* the dead filter is never at work, and never written to */
	f = (RuntimeFilter *) runtimefilter_dead();
	assert_true(runtimefilter_resting(f));
	runtimefilter_rest(f, 1000000);
	assert_true(f->dead == 1 && f->rest_rows == 0 && f->seen == 0);
}

/* the Staged Scan's inline hash is the hash opfamilies' own */
static void
test__hash_int__matches_hash_functions(void **state)
{
	static const int64 vals[] = {
		0, 1, -1, 42, -42, 32767, -32768, 2147483647, -INT64CONST(2147483648),
		INT64CONST(4294967296), -INT64CONST(4294967297),
		INT64CONST(0x7fffffffffffffff), -INT64CONST(0x7fffffffffffffff) - 1,
		INT64CONST(1234567890123456789)
	};

	for (int i = 0; i < lengthof(vals); i++)
	{
		int64		v = vals[i];

		assert_true(runtimefilter_hash_int(v, 8) ==
					call_hash(hashint8, Int64GetDatum(v)));
		if (v >= PG_INT32_MIN && v <= PG_INT32_MAX)
			assert_true(runtimefilter_hash_int(v, 4) ==
						call_hash(hashint4, Int32GetDatum((int32) v)));
		if (v >= PG_INT16_MIN && v <= PG_INT16_MAX)
			assert_true(runtimefilter_hash_int(v, 2) ==
						call_hash(hashint2, Int16GetDatum((int16) v)));
	}

	/* and a key hashed either way is found in a filter of the join's values */
	{
		RuntimeFilterBuilder *b = runtimefilter_create(1000, TopMemoryContext);
		RuntimeFilter *f;

		for (int64 v = -500; v < 500; v++)
			runtimefilter_insert(b, call_hash(hashint8, Int64GetDatum(v * 1000003)));
		f = (RuntimeFilter *) runtimefilter_finish(b);
		for (int64 v = -500; v < 500; v++)
			assert_true(runtimefilter_contains(f, runtimefilter_hash_int(v * 1000003, 8)));
		pfree(f);
	}
}

/* the keys a finished filter holds, read off its fill, within a tenth */
static void
test__estimated_keys__reads_the_fill(void **state)
{
	static const uint32 ns[] = {100, 1000, 10000, 100000};

	reset_gucs();
	for (int t = 0; t < lengthof(ns); t++)
	{
		RuntimeFilter *f = (RuntimeFilter *) runtimefilter_finish(build(ns[t], 0, ns[t]));
		double		est = runtimefilter_estimated_keys((struct varlena *) f);

		printf("estimate: %u keys read as %.0f\n", ns[t], est);
		assert_true(est > ns[t] * 0.9 && est < ns[t] * 1.1);
		pfree(f);
	}

	/* rows over few keys are read as the keys, not the rows */
	{
		RuntimeFilterBuilder *b = runtimefilter_create(1000, TopMemoryContext);
		RuntimeFilter *f;

		for (uint32 i = 0; i < 100000; i++)
			runtimefilter_insert(b, hv(i % 100));
		f = (RuntimeFilter *) runtimefilter_finish(b);
		assert_true(runtimefilter_estimated_keys((struct varlena *) f) < 120);
		pfree(f);
	}
}

/*
 * A filter sized for far more keys than it holds folds down to the size its
 * keys need and keeps every one of them; one sized right is left alone.
 */
static void
test__fold__shrinks_to_the_keys(void **state)
{
	RuntimeFilter *f;
	RuntimeFilter *g;

	reset_gucs();

	/* a thousand keys in a filter for a million: 8 kB is all they need */
	f = (RuntimeFilter *) runtimefilter_finish(build(1e6, 0, 1000));
	assert_int_equal(f->log2_blocks, 16);
	g = (RuntimeFilter *) runtimefilter_fold((struct varlena *) f);
	assert_int_equal(g->log2_blocks, 8);
	assert_int_equal(VARSIZE(g), RF_SIZE(8));
	assert_true(runtimefilter_check((struct varlena *) g) == (struct varlena *) g);
	for (uint32 i = 0; i < 1000; i++)
		assert_true(runtimefilter_contains(g, hv(i)));
	printf("folded: fill %.4f, %u false positives in 1000000\n",
		   fill_of(g), false_positives(g, 1000000));
	assert_true(false_positives(g, 1000000) < 100);
	pfree(g);

	/* 20000 keys in the 4 MB cap: to the 64 kB they need */
	f = (RuntimeFilter *) runtimefilter_finish(build(1e9, 0, 20000));
	assert_int_equal(f->log2_blocks, 17);
	g = (RuntimeFilter *) runtimefilter_fold((struct varlena *) f);
	assert_int_equal(g->log2_blocks, 11);
	for (uint32 i = 0; i < 20000; i++)
		assert_true(runtimefilter_contains(g, hv(i)));
	assert_true(false_positives(g, 1000000) < 20000);
	pfree(g);

	/* sized for its keys: as it is */
	f = (RuntimeFilter *) runtimefilter_finish(build(100000, 0, 100000));
	assert_true(runtimefilter_fold((struct varlena *) f) == (struct varlena *) f);
	assert_int_equal(f->log2_blocks, 13);
	pfree(f);
}

/* an overfull builder started over at its row count comes out right */
static void
test__restart__resizes_for_the_rows(void **state)
{
	RuntimeFilterBuilder *b;
	RuntimeFilter *f;

	reset_gucs();
	b = build(10, 0, 20000);		/* sized for ten, fed twenty thousand */
	assert_true(runtimefilter_overfull(b));
	assert_true(runtimefilter_builder_rows(b) == 20000);

	runtimefilter_restart(b, (double) runtimefilter_builder_rows(b));
	assert_true(runtimefilter_builder_rows(b) == 0);
	assert_int_equal(b->filter->log2_blocks, 11);
	assert_true(fill_of(b->filter) == 0.0);
	for (uint32 i = 0; i < 20000; i++)
		runtimefilter_insert(b, hv(i));
	assert_false(runtimefilter_overfull(b));

	f = (RuntimeFilter *) runtimefilter_finish(b);
	assert_true(f != NULL);
	assert_int_equal(f->nkeys, 20000);
	for (uint32 i = 0; i < 20000; i++)
		assert_true(runtimefilter_contains(f, hv(i)));
	assert_true(false_positives(f, 1000000) < 20000);
	pfree(f);
}

/*
 * The exact form over integer keys: a bitmap over their range, chosen when
 * no larger than the Bloom filter would be, exact on members and
 * non-members alike; a Bloom filter tagged with the range rejects a key
 * outside it without a hash.
 */
static void
test__bitmap__exact_over_the_range(void **state)
{
	RuntimeFilter *f;

	reset_gucs();

	/* 100 keys spanning 5000 values: a 5000-bit bitmap beats the 8 kB Bloom filter */
	assert_true(runtimefilter_bitmap_worth(3, 4953, 100));
	/* 100 keys spanning a million: not when the bitmap would be the larger */
	assert_false(runtimefilter_bitmap_worth(0, 1000000, 100));
	/* a range beyond the size limit: never */
	assert_false(runtimefilter_bitmap_worth(0, INT64CONST(1) << 40, 1e9));
	assert_true(runtimefilter_bitmap_create(0, INT64CONST(1) << 40, TopMemoryContext) == NULL);

	f = (RuntimeFilter *) runtimefilter_bitmap_create(3, 4953, TopMemoryContext);
	assert_true(f != NULL);
	assert_int_equal(f->flags, RF_FLAG_BITMAP);
	assert_int_equal(f->log2_blocks, 5);	/* 32 blocks, 8192 bits >= 4951 */
	assert_true(runtimefilter_check((struct varlena *) f) == (struct varlena *) f);
	for (int64 k = 3; k <= 4953; k += 50)
		runtimefilter_bitmap_set((struct varlena *) f, k);
	assert_int_equal(f->nkeys, 100);
	assert_true(runtimefilter_estimated_keys((struct varlena *) f) == 100.0);
	for (int64 k = -10; k <= 5000; k++)
	{
		bool		member = (k >= 3 && k <= 4953 && (k - 3) % 50 == 0);

		assert_true(runtimefilter_test_int(f, k, 4) == member);
		assert_true(runtimefilter_test_int(f, k, 8) == member);
	}
	/* a bitmap is sized by its range: not folded */
	assert_true(runtimefilter_fold((struct varlena *) f) == (struct varlena *) f);
	pfree(f);

	/* a Bloom filter tagged with the range: outside it, no, without a hash */
	f = (RuntimeFilter *) runtimefilter_finish(build(1000, 0, 1000));
	runtimefilter_set_range((struct varlena *) f, 100, 200);
	assert_int_equal(f->flags, RF_FLAG_INTRANGE);
	assert_false(runtimefilter_test_int(f, 99, 4));
	assert_false(runtimefilter_test_int(f, 201, 8));
	/* inside it, the Bloom filter answers (hv(i) is the int4 key i's hash) */
	assert_true(runtimefilter_test_int(f, 150, 4));
	pfree(f);
}

/* bitmaps merge only over the same range; a tagged Bloom filter's range widens */
static void
test__merge__ranges(void **state)
{
	RuntimeFilter *a;
	RuntimeFilter *b;

	reset_gucs();
	a = (RuntimeFilter *) runtimefilter_bitmap_create(0, 1000, TopMemoryContext);
	b = (RuntimeFilter *) runtimefilter_bitmap_create(0, 1000, TopMemoryContext);
	runtimefilter_bitmap_set((struct varlena *) a, 10);
	runtimefilter_bitmap_set((struct varlena *) b, 20);
	assert_true(runtimefilter_merge((struct varlena *) a, (struct varlena *) b));
	assert_true(runtimefilter_test_int(a, 10, 4) && runtimefilter_test_int(a, 20, 4));
	assert_false(runtimefilter_test_int(a, 30, 4));
	assert_int_equal(a->nkeys, 2);
	b->lo = 1;
	assert_false(runtimefilter_merge((struct varlena *) a, (struct varlena *) b));
	pfree(a);
	pfree(b);

	a = (RuntimeFilter *) runtimefilter_finish(build(20000, 0, 10000));
	b = (RuntimeFilter *) runtimefilter_finish(build(20000, 500000, 10000));
	runtimefilter_set_range((struct varlena *) a, 0, 9999);
	runtimefilter_set_range((struct varlena *) b, 500000, 509999);
	assert_true(runtimefilter_merge((struct varlena *) a, (struct varlena *) b));
	assert_true(a->lo == 0 && a->hi == 509999);
	pfree(a);
	pfree(b);
}

int
main(int argc, char *argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
		unit_test(test__create__sizes_by_estimate_and_limit),
		unit_test(test__fits__six_bits_per_key),
		unit_test(test__contains__no_false_negatives),
		unit_test(test__sparse__million_sized_thousand_filled),
		unit_test(test__sized__false_positive_rate),
		unit_test(test__finish__drops_overfull_filter),
		unit_test(test__finish__duplicates_do_not_fill),
		unit_test(test__merge__union_of_both_key_sets),
		unit_test(test__merge__refuses_unlike_filters),
		unit_test(test__check__rejects_what_is_not_a_filter),
		unit_test(test__probe_count__rests_and_wakes),
		unit_test(test__hash_int__matches_hash_functions),
		unit_test(test__estimated_keys__reads_the_fill),
		unit_test(test__fold__shrinks_to_the_keys),
		unit_test(test__restart__resizes_for_the_rows),
		unit_test(test__bitmap__exact_over_the_range),
		unit_test(test__merge__ranges)
	};

	MemoryContextInit();

	return run_tests(tests);
}
