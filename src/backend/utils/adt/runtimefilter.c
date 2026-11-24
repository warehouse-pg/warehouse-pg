/*-------------------------------------------------------------------------
 *
 * runtimefilter.c
 *	  Runtime filters for hash joins: the filter (a split-block Bloom filter
 *	  over join-key hash values), the probe of the clause the planner puts
 *	  in the probe scan's qual (a RuntimeFilterExpr), and the helpers that
 *	  recognize that clause.
 *
 * Once a hash join has built its hash table it knows exactly which join
 * keys the inner side holds. When the probe side's scan runs in the same
 * slice, the join hands that scan a Bloom filter over those keys through an
 * exec Param -- set by nodeHashjoin.c, read by the clause planrtfilter.c
 * adds to the scan's qual -- and the scan drops the rows that cannot have a
 * match before they are assembled, projected or handed up to the join. On an
 * AOCO table the clause can lead a Staged Scan, so the other columns are not
 * even decompressed for those rows.
 *
 * The filter is built from the same 32-bit hash values the hash table is
 * built with (ExecHashGetHashValue()), and the probe computes the value with
 * the hash function of the probe side's operand type -- the one the join
 * itself would have applied to the row. So a probe costs one hash function
 * call, which the join had to pay anyway, plus one cache line.
 *
 * The filter is a split-block Bloom filter: 256-bit blocks of eight 32-bit
 * words, one bit set in each word, so that a probe touches a single cache
 * line. It is sized from the planner's estimate of the inner rows at about
 * 16 bits per key (roughly 1% false positives) and capped by
 * whpg_runtime_filter_max_size. The rows that arrive decide what becomes of
 * it (nodeHashjoin.c): a filter the estimate left too small -- more than
 * RF_MAX_FILL of its bits set -- is built again from the hash table at the
 * size the rows call for, when the table is all in memory and that size is
 * within the limit, and dropped otherwise (the Param stays NULL) rather than
 * let a dense filter cost probes for nothing; one the estimate left too
 * large is folded down to the size its keys need (runtimefilter_fold()). A
 * false positive only costs the hash table probe the join would have made
 * anyway; a NULL key is rejected, since a strict hash operator never
 * matches it.
 *
 * Portions Copyright (c) 2026, WarehousePG contributors.
 *
 * IDENTIFICATION
 *	    src/backend/utils/adt/runtimefilter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "executor/execExpr.h"
#include "utils/runtimefilter.h"

/* GUCs */
bool		whpg_enable_runtime_filter = true;
int			whpg_runtime_filter_max_size = 4096;	/* kB */
bool		whpg_runtime_filter_force = false;

#define RF_BITS_PER_KEY			16	/* sizing target */
#define RF_MIN_BITS_PER_KEY		6	/* planner gate: fewer and no filter is planned */
#define RF_MIN_LOG2_BLOCKS		8	/* 256 blocks, 8 kB */
#define RF_MAX_LOG2_BLOCKS		24	/* 512 MB, within MaxAllocSize */

/*
 * A finished filter with more than this fraction of its bits set is dropped:
 * with one bit per word in eight words, the false positive rate is about the
 * fill ratio to the eighth power, 17% here, and climbing fast beyond. Judged
 * on the bits actually set, not on the tuples inserted, since duplicate keys
 * set no new bits.
 */
#define RF_MAX_FILL				0.8

#define RF_HEADER_SIZE	offsetof(RuntimeFilter, blocks)
#define RF_SIZE(log2)	(RF_HEADER_SIZE + \
						 (((Size) 1 << (log2)) * RF_BLOCK_WORDS * sizeof(uint32)))

struct RuntimeFilterBuilder
{
	RuntimeFilter *filter;
	uint64		nkeys;
};

/* one odd multiplier per word of a block */
const uint32 runtimefilter_salt[RF_BLOCK_WORDS] = {
	0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
	0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U
};

static Size
rf_max_bits(void)
{
	return (Size) whpg_runtime_filter_max_size * 1024 * 8;
}

/* Can a filter over this many keys stay within the size limit and useful? */
bool
runtimefilter_fits(double nkeys)
{
	return nkeys * RF_MIN_BITS_PER_KEY <= (double) rf_max_bits();
}

/*
 * The block count, as its log2, of a filter for nkeys keys: RF_BITS_PER_KEY
 * bits per key, a power of two of blocks, never below RF_MIN_LOG2_BLOCKS and
 * never above whpg_runtime_filter_max_size.
 */
static int
rf_log2_for(double nkeys)
{
	int			log2 = RF_MIN_LOG2_BLOCKS;
	double		want_bits = Max(nkeys, 1.0) * RF_BITS_PER_KEY;
	Size		max_bits = rf_max_bits();

	while (log2 < RF_MAX_LOG2_BLOCKS &&
		   ((Size) 1 << log2) * RF_BLOCK_BITS < want_bits &&
		   ((Size) 1 << (log2 + 1)) * RF_BLOCK_BITS <= max_bits &&
		   RF_SIZE(log2 + 1) <= MaxAllocSize)
		log2++;
	return log2;
}

/* an empty filter of 2^log2 blocks in cxt */
static RuntimeFilter *
rf_alloc(int log2, MemoryContext cxt)
{
	Size		size = RF_SIZE(log2);
	RuntimeFilter *f = (RuntimeFilter *) MemoryContextAllocZero(cxt, size);

	SET_VARSIZE(f, size);
	f->magic = RF_MAGIC;
	f->log2_blocks = log2;
	f->window = RF_ADAPT_JUDGE_ROWS;
	f->rest_windows = RF_REST_MIN_WINDOWS;
	return f;
}

/*
 * The rows the filter was to be judged over have been probed
 * (runtimefilter_probe_count()): rest a filter that let nearly all of them
 * through, twice as long as the last time if this was the sample after a
 * rest; one that earned its keep is at work, and its next rest the
 * shortest.
 */
void
runtimefilter_window_end(RuntimeFilter *f)
{
	if ((double) f->passed > RF_REST_PASS_RATIO * (double) f->seen)
	{
		f->rest_rows = (int64) f->rest_windows * RF_ADAPT_WINDOW;
		f->rest_windows = Min(f->rest_windows * 2, RF_REST_MAX_WINDOWS);
#ifdef FAULT_INJECTOR
		SIMPLE_FAULT_INJECTOR("runtime_filter_rested");
#endif
	}
	else
	{
		f->window = RF_ADAPT_JUDGE_ROWS;
		f->rest_windows = RF_REST_MIN_WINDOWS;
	}
	f->seen = 0;
	f->passed = 0;
}

/* A rest is over (runtimefilter_rest()): one window is probed as a sample. */
void
runtimefilter_wake(RuntimeFilter *f)
{
	f->rest_rows = 0;
	f->window = RF_ADAPT_WINDOW;
	f->seen = 0;
	f->passed = 0;
#ifdef FAULT_INJECTOR
	SIMPLE_FAULT_INJECTOR("runtime_filter_woken");
#endif
}

/* the share of the filter's bits that are set */
static double
rf_fill(const RuntimeFilter *f)
{
	Size		nbits = ((Size) 1 << f->log2_blocks) * RF_BLOCK_BITS;

	return (double) pg_popcount((const char *) f->blocks, nbits / 8) /
		(double) nbits;
}

/*
 * Start a filter sized for est_nkeys keys, allocated in cxt (the finished
 * filter stays there; the join frees it when it rebuilds or ends).
 */
RuntimeFilterBuilder *
runtimefilter_create(double est_nkeys, MemoryContext cxt)
{
	RuntimeFilterBuilder *b;

	b = (RuntimeFilterBuilder *) MemoryContextAlloc(cxt, sizeof(RuntimeFilterBuilder));
	b->filter = rf_alloc(rf_log2_for(est_nkeys), cxt);
	b->nkeys = 0;
	return b;
}

/* Would runtimefilter_finish() drop this filter as too full? */
bool
runtimefilter_overfull(const RuntimeFilterBuilder *b)
{
	return rf_fill(b->filter) > RF_MAX_FILL;
}

/* Throw the builder and its filter away: another form was chosen. */
void
runtimefilter_discard(RuntimeFilterBuilder *b)
{
	pfree(b->filter);
	pfree(b);
}

/*
 * The width of the keys a hash support function hashes as integers, for a
 * filter that may hold the keys' range or a bitmap over it: 2, 4 or 8 for
 * hashint2, hashint4 (int4 and date), hashint8 (int8) and timestamp_hash
 * (timestamp and timestamptz; it is hashint8 on the value), 0 for any other
 * function.
 */
int
runtimefilter_int_width(Oid hashproc)
{
	switch (hashproc)
	{
		case F_HASHINT2:
			return 2;
		case F_HASHINT4:
			return 4;
		case F_HASHINT8:
		case F_TIMESTAMP_HASH:
			return 8;
	}
	return 0;
}

/*
 * The bitmap over [lo, hi]: the number of blocks it takes, as a log2, or -1
 * when the range is wider than whpg_runtime_filter_max_size allows.
 */
static int
rf_bitmap_log2(int64 lo, int64 hi)
{
	double		span = (double) hi - (double) lo + 1.0;
	int			log2 = 0;

	if (span <= 0 || span > (double) rf_max_bits())
		return -1;
	while (((double) ((Size) 1 << log2)) * RF_BLOCK_BITS < span)
		log2++;
	return log2;
}

/*
 * Is a bitmap over [lo, hi] worth having over a Bloom filter for nkeys
 * keys? When it is no larger: exact, and a probe is a subtraction and a bit
 * test instead of a hash.
 */
bool
runtimefilter_bitmap_worth(int64 lo, int64 hi, double nkeys)
{
	int			log2 = rf_bitmap_log2(lo, hi);

	return log2 >= 0 && log2 <= rf_log2_for(nkeys);
}

struct varlena *
runtimefilter_bitmap_create(int64 lo, int64 hi, MemoryContext cxt)
{
	int			log2 = rf_bitmap_log2(lo, hi);
	RuntimeFilter *f;

	if (log2 < 0)
		return NULL;
	f = rf_alloc(log2, cxt);
	f->flags = RF_FLAG_BITMAP;
	f->lo = lo;
	f->hi = hi;
	return (struct varlena *) f;
}

void
runtimefilter_bitmap_set(struct varlena *v, int64 key)
{
	RuntimeFilter *f = (RuntimeFilter *) v;
	uint64		bit;

	Assert(f->flags & RF_FLAG_BITMAP);
	Assert(key >= f->lo && key <= f->hi);
	bit = (uint64) (key - f->lo);
	f->blocks[bit >> 5] |= 1U << (bit & 31);
	f->nkeys = (uint32) Min((uint64) f->nkeys + 1, PG_UINT32_MAX);
}

/* Tag a Bloom filter over integer keys with their range: a probe outside it is rejected before hashing. */
void
runtimefilter_set_range(struct varlena *v, int64 lo, int64 hi)
{
	RuntimeFilter *f = (RuntimeFilter *) v;

	Assert(!(f->flags & RF_FLAG_BITMAP));
	f->flags |= RF_FLAG_INTRANGE;
	f->lo = lo;
	f->hi = hi;
}

/* the rows inserted so far */
uint64
runtimefilter_builder_rows(const RuntimeFilterBuilder *b)
{
	return b->nkeys;
}

/*
 * Start the builder over with an empty filter sized for nkeys keys, in the
 * memory context of the one it had: for a filter the estimate left too
 * small, to be fed again from the hash table (nodeHash.c).
 */
void
runtimefilter_restart(RuntimeFilterBuilder *b, double nkeys)
{
	MemoryContext cxt = GetMemoryChunkContext(b->filter);

	pfree(b->filter);
	b->filter = rf_alloc(rf_log2_for(nkeys), cxt);
	b->nkeys = 0;
}

void
runtimefilter_insert(RuntimeFilterBuilder *b, uint32 hashvalue)
{
	uint64		h = runtimefilter_mix64(hashvalue);
	uint32	   *block = runtimefilter_block(b->filter, h);
	uint32		hm = (uint32) h;

	for (int i = 0; i < RF_BLOCK_WORDS; i++)
		block[i] |= 1U << ((hm * runtimefilter_salt[i]) >> 27);
	b->nkeys++;
}

/*
 * The finished filter, or NULL if it is too full to be worth probing (the
 * builder is freed either way). Fullness is measured on the bits set, so an
 * inner side of many rows over few distinct keys keeps its filter however
 * low the row estimate it was sized from.
 */
struct varlena *
runtimefilter_finish(RuntimeFilterBuilder *b)
{
	RuntimeFilter *f = b->filter;

	if (rf_fill(f) > RF_MAX_FILL)
	{
		pfree(f);
		f = NULL;
	}
	else
		f->nkeys = (uint32) Min(b->nkeys, PG_UINT32_MAX);
	pfree(b);
	return (struct varlena *) f;
}

/*
 * About how many distinct keys a finished filter holds, read off its fill:
 * a key sets one bit in each word of one block, so n keys over nbits bits
 * set about nbits * (1 - exp(-8 n / nbits)) of them. Rows over a key already
 * in set no new bits, so this counts keys where the builder counted rows; a
 * filter with every bit set reads as holding as many keys as it has bits.
 */
double
runtimefilter_estimated_keys(const struct varlena *v)
{
	const RuntimeFilter *f = (const RuntimeFilter *) v;
	Size		nbits = ((Size) 1 << f->log2_blocks) * RF_BLOCK_BITS;
	double		fill = rf_fill(f);

	if (f->flags & RF_FLAG_BITMAP)	/* exact: one bit per key */
		return fill * (double) nbits;
	if (fill >= 1.0)
		return (double) nbits;
	if (fill <= 0.0)
		return 0.0;
	return -log(1.0 - fill) * (double) nbits / RF_BLOCK_WORDS;
}

/*
 * Fold a finished filter the estimate left far too large -- a few keys in
 * megabytes, every probe a cache miss -- down to the size its keys call for
 * (RF_BITS_PER_KEY per key, as it would have been sized had the estimate
 * been right). Each halving ORs the upper half of the blocks into the
 * lower: a key's block is the top half of its hash masked to the block
 * count, so in the halved filter it is the old block modulo the new count,
 * and every key stays in. The filter is shrunk in its own memory context
 * and the new pointer returned; a filter already at the size of its keys, or
 * a bitmap (sized by its range), is returned as it is.
 */
struct varlena *
runtimefilter_fold(struct varlena *v)
{
	RuntimeFilter *f = (RuntimeFilter *) v;
	int			target;

	if (f->flags & RF_FLAG_BITMAP)
		return v;
	target = rf_log2_for(runtimefilter_estimated_keys(v));
	if (target >= f->log2_blocks)
		return v;
	while (f->log2_blocks > target)
	{
		Size		half = ((Size) 1 << (f->log2_blocks - 1)) * RF_BLOCK_WORDS;

		for (Size i = 0; i < half; i++)
			f->blocks[i] |= f->blocks[half + i];
		f->log2_blocks--;
	}
	f = (RuntimeFilter *) repalloc(f, RF_SIZE(f->log2_blocks));
	SET_VARSIZE(f, RF_SIZE(f->log2_blocks));
	return (struct varlena *) f;
}

/*
 * OR 'other' into 'into': the filter over the union of both key sets. Two
 * filters merge when they were sized alike, which filters built from the
 * same plan estimate are; the caller sizes them so. Returns false, leaving
 * 'into' alone, if they were not. This is what a filter spanning several
 * segments is made of, once a transport brings the parts together.
 */
bool
runtimefilter_merge(struct varlena *into, const struct varlena *other)
{
	RuntimeFilter *a = (RuntimeFilter *) into;
	const RuntimeFilter *b = (const RuntimeFilter *) other;
	Size		nwords;

	if (a->magic != RF_MAGIC || b->magic != RF_MAGIC ||
		a->log2_blocks != b->log2_blocks || a->flags != b->flags)
		return false;
	/* a bitmap is over its range; a Bloom filter's range widens */
	if (a->flags & RF_FLAG_BITMAP)
	{
		if (a->lo != b->lo || a->hi != b->hi)
			return false;
	}
	else if (a->flags & RF_FLAG_INTRANGE)
	{
		a->lo = Min(a->lo, b->lo);
		a->hi = Max(a->hi, b->hi);
	}

	nwords = ((Size) 1 << a->log2_blocks) * RF_BLOCK_WORDS;
	for (Size i = 0; i < nwords; i++)
		a->blocks[i] |= b->blocks[i];
	a->nkeys = (uint32) Min((uint64) a->nkeys + b->nkeys, PG_UINT32_MAX);
	return true;
}

/*
 * The filter a join publishes when it has none to publish -- not built,
 * dropped as too full: a filter that is never probed, so that the scans
 * reading the Param know the join is done deciding (a Param still NULL
 * means the filter is yet to come) and give the clause up for good
 * (SeqNext). It lets every row through, as a NULL Param does, and is never
 * written to, since every probe tests 'dead' first and it is never at rest.
 */
struct varlena *
runtimefilter_dead(void)
{
	static RuntimeFilter *dead = NULL;

	if (dead == NULL)
	{
		dead = rf_alloc(0, TopMemoryContext);
		dead->dead = 1;
	}
	return (struct varlena *) dead;
}

/*
 * The filter a Param holds, or NULL if what it holds is not a filter the
 * hash join built -- a guard against an executor or planner fault, since
 * only the planner writes the clause that reads it. NULL lets every row
 * through, as a Param the join has not set does: a filter only ever drops
 * rows the join would drop, so one that cannot be trusted is not an error,
 * it is no filter. The Staged Scan's fast path reads the Param the same way.
 */
struct varlena *
runtimefilter_check(struct varlena *v)
{
	RuntimeFilter *f = (RuntimeFilter *) v;

	if (VARSIZE(v) < RF_HEADER_SIZE ||
		f->magic != RF_MAGIC ||
		f->log2_blocks > RF_MAX_LOG2_BLOCKS ||
		VARSIZE(v) != RF_SIZE(f->log2_blocks))
	{
		elog(DEBUG1, "runtime filter ignored: the parameter does not hold a filter");
		return NULL;
	}
	return v;
}

/*
 * The probe of a RuntimeFilterExpr (primnodes.h), the clause the planner
 * puts in the probe scan's qual: true if the key may be in the filter. The
 * executor compiles the node to steps of its own (execExpr.c) whose state
 * this is: EEOP_RUNTIME_FILTER_LIVE asks whether there is a filter to probe
 * and jumps over the rest when there is not, the steps between leave the
 * key in the state, EEOP_RUNTIME_FILTER probes. The clause rests on no
 * catalog entry.
 *
 * A NULL filter -- none was built, or it was dropped -- accepts every row. A
 * NULL key is rejected when the hash operator is strict, since the join
 * never matches it then; a non-strict operator hashes it as 0, as
 * ExecHashGetHashValue() does (the planner gives such a join no filter; this
 * keeps the probe true to the join). A filter that passes nearly every row
 * is rested, and probed again later in the scan
 * (runtimefilter_probe_count() in runtimefilter.h). The key is hashed with
 * the hash operator's left-hand hash support function, exactly as
 * ExecHashTableCreate() hashes the probe side, looked up once per plan node.
 *
 * The probe cannot fail. The node is checked once per plan node, as a guard
 * against a planner or executor fault (only the planner writes one), and a
 * clause that fails a check lets every row through from then on: a filter
 * only ever drops rows the join would drop, so a clause that cannot be
 * trusted is no filter, not an error. The fault is logged, once per plan
 * node.
 */
static void
rf_probe_give_up(RuntimeFilterProbeState *probe, const char *why)
{
	probe->unusable = true;
	ereport(LOG,
			(errmsg("runtime filter ignored: %s", why)));
}

/*
 * The one test of a probe key against a hash operator, for the planner pass
 * (before it writes a clause), the probe (before it trusts one) and the
 * Staged Scan's compile step (before it takes one onto its fast path).
 *
 * The key is hashed by the operator's left-hand hash function, as
 * ExecHashTableCreate() hashes the probe side, so the key must be what that
 * function expects: the operator's left-hand operand type (a date is hashed
 * by hashint4, so the key's own type would be the wrong thing to compare
 * with), and one the operator hashes at all (op_hashjoinable(): an array's
 * elements need a hash function). Only a hash operator's own support
 * function is ever returned.
 */
bool
runtimefilter_key_hashable(Oid hashop, Oid keytype, Oid *hashproc, bool *strict)
{
	Oid			lhsproc;
	Oid			rhsproc;
	Oid			lefttype;
	Oid			righttype;

	if (!OidIsValid(hashop) || !OidIsValid(keytype))
		return false;
	if (!op_hashjoinable(hashop, keytype) ||
		!get_op_hash_functions(hashop, &lhsproc, &rhsproc))
		return false;
	op_input_types(hashop, &lefttype, &righttype);
	if (!IsBinaryCoercible(keytype, lefttype))
		return false;

	if (hashproc)
		*hashproc = lhsproc;
	if (strict)
		*strict = op_strict(hashop);
	return true;
}

RuntimeFilterProbeState *
runtimefilter_probe_init(const RuntimeFilterExpr *rfexpr)
{
	RuntimeFilterProbeState *probe;
	Oid			keytype = exprType((Node *) rfexpr->key);
	Oid			hashproc;

	probe = (RuntimeFilterProbeState *) palloc0(sizeof(RuntimeFilterProbeState));
	probe->paramid = rfexpr->paramid;
	probe->collation = rfexpr->collation;

	if (!runtimefilter_key_hashable(rfexpr->hashop, keytype,
									&hashproc, &probe->opstrict))
		rf_probe_give_up(probe,
						 psprintf("operator %s cannot hash a key of type %s",
								  OidIsValid(rfexpr->hashop) ?
								  format_operator(rfexpr->hashop) : "(none)",
								  OidIsValid(keytype) ?
								  format_type_be(keytype) : "unknown"));
	else
	{
		fmgr_info(hashproc, &probe->hashfn);
		probe->intlen = runtimefilter_int_width(hashproc);
	}
	return probe;
}

/* the integer value of a key hashed as an integer of 'len' bytes */
static inline int64
rf_int_key(Datum value, int len)
{
	if (len == 8)
		return DatumGetInt64(value);
	if (len == 4)
		return (int64) DatumGetInt32(value);
	return (int64) DatumGetInt16(value);
}

/*
 * Is there a filter to probe? False for a clause not to be trusted, a Param
 * not set (no filter built yet), a Param that does not hold a filter, the
 * dead filter, or a filter at rest, which is told of the row that goes by
 * unprobed. The step before the key's evaluation asks
 * (ExecEvalRuntimeFilterLive()), so that none of these costs a row more
 * than the question; runtimefilter_probe() is only reached through it.
 */
bool
runtimefilter_probe_live(RuntimeFilterProbeState *probe, ExprContext *econtext)
{
	ParamExecData *prm;
	RuntimeFilter *f;

	if (probe->unusable || econtext->ecxt_param_exec_vals == NULL)
		return false;
	prm = &econtext->ecxt_param_exec_vals[probe->paramid];
	if (prm->isnull)
		return false;
	f = (RuntimeFilter *) runtimefilter_check((struct varlena *) DatumGetPointer(prm->value));
	if (f == NULL)
	{
		rf_probe_give_up(probe, "the parameter does not hold a filter");
		return false;
	}
	if (f->dead)
		return false;
	if (f->rest_rows > 0)
	{
		runtimefilter_rest(f, 1);
		return false;
	}
	return true;
}

bool
runtimefilter_probe(RuntimeFilterProbeState *probe, ExprContext *econtext)
{
	ParamExecData *prm;
	RuntimeFilter *f;
	uint32		hashvalue;
	bool		result;

	/* a clause not to be trusted, or an ExprContext without the Params */
	if (probe->unusable || econtext->ecxt_param_exec_vals == NULL)
		return true;

	/* no filter (none built, or dropped) */
	prm = &econtext->ecxt_param_exec_vals[probe->paramid];
	if (prm->isnull)
		return true;

	/* a strict operator never matches a NULL key: the join drops the row */
	if (probe->keynull && probe->opstrict)
	{
		probe->nremoved++;
		return false;
	}

	/* the pointer the hash join published */
	f = (RuntimeFilter *) runtimefilter_check((struct varlena *) DatumGetPointer(prm->value));
	if (f == NULL)
	{
		rf_probe_give_up(probe, "the parameter does not hold a filter");
		return true;
	}

	/* the dead filter, or one at rest, lets the rows through */
	if (runtimefilter_resting(f))
		return true;

	/*
	 * Over an integer-like key the filter may hold the keys' range, or be a
	 * bitmap over it; a key hashed as an integer is tested as one. (A
	 * non-strict operator's NULL key, hashed as 0 below, is not an integer
	 * key: the join does not give such a key a filter over integers.)
	 */
	if ((f->flags & (RF_FLAG_INTRANGE | RF_FLAG_BITMAP)) && !probe->keynull)
	{
		if (probe->intlen == 0)
		{
			rf_probe_give_up(probe, "the filter is over integer keys, the probe key is not one");
			return true;
		}
		result = runtimefilter_test_int(f, rf_int_key(probe->keyvalue, probe->intlen),
										probe->intlen);
		runtimefilter_probe_count(f, result);
		if (!result)
			probe->nremoved++;
		return result;
	}
	if (f->flags & RF_FLAG_BITMAP)
		return true;			/* a NULL key under a non-strict operator: not in a bitmap's domain */

	/* a non-strict operator's NULL key hashes as 0, as in the join */
	if (probe->keynull)
		hashvalue = 0;
	else
		hashvalue = DatumGetUInt32(FunctionCall1Coll(&probe->hashfn,
													 probe->collation,
													 probe->keyvalue));
	result = runtimefilter_contains(f, hashvalue);
	runtimefilter_probe_count(f, result);
	if (!result)
		probe->nremoved++;

	return result;
}

/*
 * The probe states of a compiled qual's runtime filter clauses, for whoever
 * owns the qual to read what the clauses removed (EXPLAIN ANALYZE's "Rows
 * Removed by Runtime Filter"). The opcode is read through ExecEvalStepOp(),
 * since a prepared interpreter replaces it with a jump address.
 */
List *
runtimefilter_qual_probes(ExprState *qual)
{
	List	   *probes = NIL;

	if (qual == NULL)
		return NIL;
	for (int i = 0; i < qual->steps_len; i++)
	{
		ExprEvalStep *op = &qual->steps[i];

		if (ExecEvalStepOp(qual, op) == EEOP_RUNTIME_FILTER)
			probes = lappend(probes, op->d.rtfilter.probe);
	}
	return probes;
}

/* What those probes have rejected so far, together. */
double
runtimefilter_probes_removed(List *probes)
{
	double		removed = 0;
	ListCell   *lc;

	foreach(lc, probes)
		removed += (double) ((RuntimeFilterProbeState *) lfirst(lc))->nremoved;
	return removed;
}

/*
 * Build the clause: a RuntimeFilterExpr over the Param 'paramid', the key,
 * the join's hash operator ('hashop', with the key as its left-hand operand)
 * and the hash clause's input collation, which the hash function is called
 * with on both sides of the join.
 */
Expr *
runtimefilter_make_clause(int paramid, Expr *key, Oid hashop,
						  Oid collation, double selectivity)
{
	RuntimeFilterExpr *rfexpr = makeNode(RuntimeFilterExpr);

	rfexpr->paramid = paramid;
	rfexpr->key = key;
	rfexpr->hashop = hashop;
	rfexpr->collation = collation;
	rfexpr->selectivity = selectivity;
	return (Expr *) rfexpr;
}

/*
 * Is this the clause the planner builds? Only the planner writes a
 * RuntimeFilterExpr -- SQL has no syntax for one -- so a Staged Scan may
 * take the Param of a clause this accepts for one a hash join sets.
 */
bool
runtimefilter_is_clause(const Node *node)
{
	return node != NULL && IsA(node, RuntimeFilterExpr);
}

/* the key expression of a clause runtimefilter_is_clause() accepted */
Node *
runtimefilter_clause_key(const Node *clause)
{
	Assert(runtimefilter_is_clause(clause));
	return (Node *) ((const RuntimeFilterExpr *) clause)->key;
}

/* split a qual into its runtime filter clauses and the rest, order kept */
void
runtimefilter_split_qual(List *qual, List **filters, List **rest)
{
	ListCell   *lc;

	*filters = NIL;
	*rest = NIL;
	foreach(lc, qual)
	{
		Node	   *clause = (Node *) lfirst(lc);

		if (runtimefilter_is_clause(clause))
			*filters = lappend(*filters, clause);
		else
			*rest = lappend(*rest, clause);
	}
}

/* The reason behind a HashJoin's rf_decline, for EXPLAIN. */
const char *
runtimefilter_decline_reason(int code)
{
	switch ((RfDecline) code)
	{
		case RF_DECLINE_NONE:
			return NULL;
		case RF_DECLINE_JOIN_TYPE:
			return "the join keeps unmatched probe rows";
		case RF_DECLINE_NULL_MATCHING:
			return "NULL keys match (IS NOT DISTINCT FROM)";
		case RF_DECLINE_SHAPE:
			return "no Hash node on the inner side";
		case RF_DECLINE_PARALLEL:
			return "parallel-aware join or scan";
		case RF_DECLINE_PROBE_MOTION:
			return "the probe rows come from another slice";
		case RF_DECLINE_PROBE_NOT_SCAN:
			return "the probe side is not a scan in this slice, or the key is not a column of it";
		case RF_DECLINE_KEY_STATS:
			return "no statistics on the probe's key columns to choose a hash key by";
		case RF_DECLINE_PROBE_SMALL:
			return "too few probe rows";
		case RF_DECLINE_UNSELECTIVE:
			return "too many probe rows expected to match";
		case RF_DECLINE_KEYS_UNSELECTIVE:
			return "no single hash key is expected to drop enough probe rows";
		case RF_DECLINE_BUILD_LARGE:
			return "the inner side needs a filter over whpg_runtime_filter_max_size";
		case RF_DECLINE_OP_NOT_HASHABLE:
			return "the hash operator has no hash support function";
		case RF_DECLINE_OP_NOT_STRICT:
			return "a non-strict hash operator, whose NULL keys may match";
		case RF_DECLINE_STAGED_SCAN:
			return "the clause would have cost the scan its Staged Scan";
	}
	return "unknown";
}
