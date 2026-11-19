/*-------------------------------------------------------------------------
 *
 * runtimefilter.h
 *	  Runtime filters for hash joins: a filter over the inner side's join
 *	  keys, handed to the probe side's scan in the same slice.
 *
 * See runtimefilter.c for the filter and the probe function, planrtfilter.c
 * for the planner pass that decides where one is used.
 *
 * Portions Copyright (c) 2026, WarehousePG contributors.
 *
 * src/include/utils/runtimefilter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RUNTIMEFILTER_H
#define RUNTIMEFILTER_H

#include "access/hash.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "utils/palloc.h"

/* GUCs, see guc_gp.c */
extern bool whpg_enable_runtime_filter;

/*
 * How far the planner pass trusts its estimates (whpg_runtime_filter_mode):
 * estimate gates a filter on them; eager plans one for every join whose
 * shape allows it and leaves the rows that arrive to decide (nodeHashjoin.c:
 * sizing, publishing, resting it while useless); auto is eager for a probe whose key
 * column has no statistics -- a table never analyzed -- and estimate
 * otherwise.
 */
typedef enum RuntimeFilterMode
{
	RF_MODE_ESTIMATE,
	RF_MODE_EAGER,
	RF_MODE_AUTO
} RuntimeFilterMode;

extern int	whpg_runtime_filter_mode;
extern int	whpg_runtime_filter_max_size;	/* kB, one filter */
extern double whpg_runtime_filter_max_selectivity;
extern int	whpg_runtime_filter_min_probe_rows;
extern double whpg_runtime_filter_disable_ratio;
extern bool whpg_runtime_filter_explain;

/*
 * Building a filter from the inner side's hash values, one insert per
 * tuple the hash table takes (nodeHash.c), published by the join
 * (nodeHashjoin.c) once the table is complete.
 */
typedef struct RuntimeFilterBuilder RuntimeFilterBuilder;

extern bool runtimefilter_fits(double nkeys);
extern RuntimeFilterBuilder *runtimefilter_create(double est_nkeys,
												  MemoryContext cxt);
extern void runtimefilter_insert(RuntimeFilterBuilder *builder,
								 uint32 hashvalue);
extern bool runtimefilter_overfull(const RuntimeFilterBuilder *builder);
extern uint64 runtimefilter_builder_rows(const RuntimeFilterBuilder *builder);
extern void runtimefilter_restart(RuntimeFilterBuilder *builder, double nkeys);
extern struct varlena *runtimefilter_finish(RuntimeFilterBuilder *builder);
extern struct varlena *runtimefilter_fold(struct varlena *filter);
extern double runtimefilter_estimated_keys(const struct varlena *filter);
extern struct varlena *runtimefilter_dead(void);
extern bool runtimefilter_merge(struct varlena *into,
								const struct varlena *other);
extern struct varlena *runtimefilter_check(struct varlena *v);

/*
 * The filter itself, as the bytea holds it, and the probe -- inline, so that
 * the AOCO Staged Scan can test a block's raw datum array against it without
 * going through fmgr (aocs_stagedscan.c), exactly as the qual's probe does
 * (runtimefilter_probe()).
 *
 * A split-block Bloom filter: 256-bit blocks of eight 32-bit words, one bit
 * set per word (k = 8), so a probe touches one cache line. The 32-bit hash
 * value is the hash table's own; it is spread to 64 bits, the top half picks
 * the block, the bottom half the bit in each word through eight odd
 * multipliers.
 *
 * Over an integer-like key (int2, int4, int8, date, timestamp, timestamptz:
 * the types hashint2/4/8 hash) the join also knows the keys' range, and
 * when the range is no wider than the Bloom filter would be, the filter is
 * a plain bitmap over it instead (RF_FLAG_BITMAP): exact, one bit per
 * value, no hash at all. A Bloom filter over such a key carries the range
 * too (RF_FLAG_INTRANGE) and rejects a value outside it before hashing.
 */
#define RF_MAGIC				0x33465257	/* "WRF3": format and version */
#define RF_BLOCK_WORDS			8
#define RF_BLOCK_BITS			(RF_BLOCK_WORDS * 32)

#define RF_FLAG_INTRANGE		0x1	/* lo/hi hold the integer keys' range */
#define RF_FLAG_BITMAP			0x2	/* blocks is a bitmap over [lo, hi] */

typedef struct RuntimeFilter
{
	int32		vl_len_;		/* varlena header (do not touch directly) */
	uint32		magic;
	uint32		log2_blocks;	/* blocks of RF_BLOCK_BITS, as a power of two */
	uint32		nkeys;			/* keys inserted, saturating */
	uint32		flags;			/* RF_FLAG_*; equal on both sides of a merge */

	/*
	 * The probe's watch on the filter's usefulness (runtimefilter_probe_count()
	 * below), kept in the filter so that everything that probes it in this
	 * process -- the scan's qual, a Staged Scan's fast path, every
	 * partition's scan under a dynamic scan -- counts into one tally and
	 * rests the filter together. State of the process that probes, not
	 * content: a fresh filter starts awake, a merge leaves it alone.
	 */
	uint32		dead;			/* runtimefilter_dead(): never probed */
	uint32		window;			/* rows the filter is judged over next */
	uint32		seen;			/* rows probed so far of those */
	uint32		passed;			/* ... and let through */
	uint32		rest_windows;	/* length of the next rest, in windows */
	int64		rest_rows;		/* > 0: resting, for this many rows more */

	/* the integer keys' range, when RF_FLAG_INTRANGE or RF_FLAG_BITMAP */
	int64		lo;
	int64		hi;

	uint32		blocks[FLEXIBLE_ARRAY_MEMBER];	/* RF_BLOCK_WORDS per block */
} RuntimeFilter;

extern const uint32 runtimefilter_salt[RF_BLOCK_WORDS];

static inline uint64
runtimefilter_mix64(uint32 hashvalue)
{
	uint64		x = hashvalue;

	x ^= x >> 33;
	x *= UINT64CONST(0xff51afd7ed558ccd);
	x ^= x >> 33;
	x *= UINT64CONST(0xc4ceb9fe1a85ec53);
	x ^= x >> 33;
	return x;
}

static inline uint32 *
runtimefilter_block(RuntimeFilter *f, uint64 h)
{
	uint32		blk = ((uint32) (h >> 32)) & ((1U << f->log2_blocks) - 1);

	return f->blocks + (Size) blk * RF_BLOCK_WORDS;
}

static inline bool
runtimefilter_contains(RuntimeFilter *f, uint32 hashvalue)
{
	uint64		h = runtimefilter_mix64(hashvalue);
	const uint32 *block = runtimefilter_block(f, h);
	uint32		hm = (uint32) h;

	for (int i = 0; i < RF_BLOCK_WORDS; i++)
	{
		if ((block[i] & (1U << ((hm * runtimefilter_salt[i]) >> 27))) == 0)
			return false;
	}
	return true;
}

/*
 * The hash value of a by-value integer-like key as the hash opfamilies
 * compute it -- hashint2(), hashint4() (date too) and hashint8() (which
 * timestamp_hash() calls) are these three lines -- for a probe that has the raw value
 * and would rather not call the function through fmgr. 'len' is the
 * column's attlen; the caller has checked that the operator's left-hand hash
 * function is the matching one of the three.
 */
static inline uint32
runtimefilter_hash_int(int64 v, int len)
{
	if (len == 8)
	{
		uint32		lohalf = (uint32) v;
		uint32		hihalf = (uint32) (v >> 32);

		lohalf ^= (v >= 0) ? hihalf : ~hihalf;
		return DatumGetUInt32(hash_uint32(lohalf));
	}
	return DatumGetUInt32(hash_uint32((int32) v));
}

/*
 * Does the integer-like key v pass the filter? Outside the keys' range, no,
 * without a hash; a bitmap answers exactly from its bit; a Bloom filter is
 * probed with the key's hash value, which is hashint2/4/8's for 'len' 2, 4
 * or 8 (the caller has checked the operator hashes so).
 */
static inline bool
runtimefilter_test_int(RuntimeFilter *f, int64 v, int len)
{
	if (f->flags & (RF_FLAG_INTRANGE | RF_FLAG_BITMAP))
	{
		if (v < f->lo || v > f->hi)
			return false;
		if (f->flags & RF_FLAG_BITMAP)
		{
			uint64		bit = (uint64) (v - f->lo);

			return (f->blocks[bit >> 5] >> (bit & 31)) & 1;
		}
	}
	return runtimefilter_contains(f, runtimefilter_hash_int(v, len));
}

/* The width (2, 4, 8) of the keys a hash support function hashes as integers, 0 for any other. */
extern int	runtimefilter_int_width(Oid hashproc);

/*
 * The exact form over an integer-like key: a bitmap over the keys' range
 * [lo, hi], built by the join once the table holds every inner row
 * (nodeHashjoin.c) and worth it when no wider than the Bloom filter for
 * nkeys keys would be. runtimefilter_bitmap_create() returns NULL for a
 * range beyond whpg_runtime_filter_max_size.
 */
extern bool runtimefilter_bitmap_worth(int64 lo, int64 hi, double nkeys);
extern struct varlena *runtimefilter_bitmap_create(int64 lo, int64 hi,
												   MemoryContext cxt);
extern void runtimefilter_bitmap_set(struct varlena *filter, int64 v);
extern void runtimefilter_set_range(struct varlena *filter, int64 lo, int64 hi);
extern void runtimefilter_discard(RuntimeFilterBuilder *builder);

/*
 * The probe watches the filter's usefulness, RF_ADAPT_JUDGE_ROWS rows at a
 * time. A filter that let more than whpg_runtime_filter_disable_ratio of
 * them through is rested: the rows go straight to the join, since every
 * probe would be a hash and a cache line for nothing. The rest is not for
 * good. The rows that pass a filter depend on where the scan is -- a table
 * loaded in key or time order has the matching rows together -- so after
 * RF_REST_MIN_WINDOWS windows' worth of rows (a window is RF_ADAPT_WINDOW
 * rows) the filter is probed for one window again. If that sample shows it
 * removing rows it is back at work, judged as before; if not, it rests
 * twice as long, up to RF_REST_MAX_WINDOWS windows. A filter that never
 * helps is thus probed for one row in RF_REST_MAX_WINDOWS + 1, and one that
 * starts to help is back within that many windows.
 *
 * The tally is the filter's own, so all who probe one filter in a process
 * share it -- a partitioned probe does not start over at every partition --
 * and a rebuilt hash table's new filter starts afresh. Whoever lets rows by
 * a resting filter unprobed tells it how many (runtimefilter_rest()): the
 * qual's probe a row at a time, a scan that has dropped the clause and the
 * Staged Scan's fast path in batches. The dead filter is never written to.
 */
#define RF_ADAPT_WINDOW			4096
#define RF_ADAPT_JUDGE_ROWS		65536
#define RF_REST_MIN_WINDOWS		16
#define RF_REST_MAX_WINDOWS		64

extern void runtimefilter_window_end(RuntimeFilter *f);
extern void runtimefilter_wake(RuntimeFilter *f);

/* is the filter to be left alone: the dead one, or one at rest? */
static inline bool
runtimefilter_resting(const RuntimeFilter *f)
{
	return f->dead || f->rest_rows > 0;
}

/* 'nrows' rows went by a resting filter unprobed */
static inline void
runtimefilter_rest(RuntimeFilter *f, int64 nrows)
{
	if (f->rest_rows > 0 && (f->rest_rows -= nrows) <= 0)
		runtimefilter_wake(f);
}

/* a row was probed, and passed or not; the caller has seen the filter awake */
static inline void
runtimefilter_probe_count(RuntimeFilter *f, bool passed)
{
	f->seen++;
	if (passed)
		f->passed++;
	if (f->seen >= f->window)
		runtimefilter_window_end(f);
}

/*
 * The clause the planner puts in the probe scan's qual: a RuntimeFilterExpr
 * (primnodes.h) over the exec Param the join sets (planrtfilter.c).
 */
extern Expr *runtimefilter_make_clause(int paramid, Expr *key, Oid hashop,
									   Oid collation, double selectivity);
extern bool runtimefilter_is_clause(const Node *node);
extern Node *runtimefilter_clause_key(const Node *clause);
extern void runtimefilter_split_qual(List *qual, List **filters, List **rest);

/*
 * Can a probe key of type 'keytype' be tested against a filter of the hash
 * values a join computed with 'hashop'? The one answer the planner pass, the
 * probe and the Staged Scan's compile step all take, so that none of them
 * can write or read a clause the others would not.
 */
extern bool runtimefilter_key_hashable(Oid hashop, Oid keytype,
									   Oid *hashproc, bool *strict);

/*
 * The executor's state for one RuntimeFilterExpr. execExpr.c compiles the
 * node to an EEOP_RUNTIME_FILTER step that owns one: the steps before it
 * leave the key in keyvalue/keynull, and runtimefilter_probe() tests it
 * against the filter the Param holds.
 */
typedef struct RuntimeFilterProbeState
{
	int			paramid;		/* the Param that holds the filter */
	Datum		keyvalue;		/* the key, evaluated by the steps before */
	bool		keynull;
	FmgrInfo	hashfn;			/* the hash operator's left-hand hash function */
	Oid			collation;
	int			intlen;			/* 2, 4, 8 when hashfn hashes integers so, else 0 */
	bool		opstrict;		/* does the hash operator reject NULL? */
	bool		unusable;		/* a check failed: every row passes */
	uint64		nremoved;		/* rows the probe rejected (EXPLAIN ANALYZE) */
} RuntimeFilterProbeState;

struct ExprContext;
extern RuntimeFilterProbeState *runtimefilter_probe_init(const RuntimeFilterExpr *rfexpr);
extern bool runtimefilter_probe_live(RuntimeFilterProbeState *probe,
									 struct ExprContext *econtext);
extern bool runtimefilter_probe(RuntimeFilterProbeState *probe,
								struct ExprContext *econtext);
struct ExprState;
extern List *runtimefilter_qual_probes(struct ExprState *qual);
extern double runtimefilter_probes_removed(List *probes);

/*
 * Why the planner gave a hash join no filter (HashJoin.rf_decline), for
 * EXPLAIN under whpg_runtime_filter_explain. 0 means a filter was planned,
 * or the join was never considered.
 */
typedef enum RfDecline
{
	RF_DECLINE_NONE = 0,
	RF_DECLINE_JOIN_TYPE,		/* a join type that keeps unmatched probe rows */
	RF_DECLINE_NULL_MATCHING,	/* IS NOT DISTINCT FROM: NULL keys match */
	RF_DECLINE_SHAPE,			/* no Hash node on the inner side */
	RF_DECLINE_PARALLEL,		/* a parallel-aware join or scan */
	RF_DECLINE_PROBE_MOTION,	/* the probe rows come from another slice */
	RF_DECLINE_PROBE_NOT_SCAN,	/* no scan found, or the key is not its column */
	RF_DECLINE_KEY_STATS,		/* several keys, no statistics to choose one by */
	RF_DECLINE_PROBE_SMALL,		/* fewer probe rows than the gate */
	RF_DECLINE_UNSELECTIVE,		/* too many probe rows expected to match */
	RF_DECLINE_KEYS_UNSELECTIVE,	/* several keys, none selective enough alone */
	RF_DECLINE_BUILD_LARGE,		/* the inner side needs a filter over the size limit */
	RF_DECLINE_OP_NOT_HASHABLE,	/* the hash operator has no hash support function */
	RF_DECLINE_OP_NOT_STRICT,	/* a non-strict hash operator: NULL keys may match */
	RF_DECLINE_STAGED_SCAN		/* the clause would have cost the scan its Staged Scan */
} RfDecline;

extern const char *runtimefilter_decline_reason(int code);

#endif							/* RUNTIMEFILTER_H */
