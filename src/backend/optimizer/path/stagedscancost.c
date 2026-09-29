/*-------------------------------------------------------------------------
 *
 * stagedscancost.c
 *	  Cost model deciding when the Staged Scan two-phase AOCO scan beats an
 *	  ordinary sequential scan, and in what order to stage a multi-column
 *	  predicate.
 *
 * Staged Scan splits an AOCO scan in two (see cdbaocsam_stagedscan.h):
 * Phase A reads only the first filter column and records the surviving row
 * numbers; Phase B fetches every other column by varblock header, per
 * surviving row, decompressing only the blocks that hold a survivor. A
 * predicate on several columns is staged inside Phase B: the remaining filter
 * columns are fetched first, in a chosen order, and a row that fails a stage
 * is dropped before any later column is touched.
 *
 * What it pays, and what it saves
 * -------------------------------
 * Writing N for the row count, U for the per-datum unpack cost, and for a
 * column c: R(c) the I/O to read it, B(c) its varblock count, dpb(c) the cost
 * of decompressing one of those, D(c) = B(c)*dpb(c). Each column fetched in
 * Phase B is asked for a set of rows S that is a subset of the survivors of
 * the stages before it; the two quantities that decide its cost are
 *
 *   H(c,S)  the number of distinct varblocks holding a member of S, which is
 *           what decides how much decompression is skipped, and
 *   rho(S)  how deep into the segfile the last member of S sits: past it the
 *           tail of the file is never touched at all.
 *
 * Then, for stages f_0 .. f_{k-1} with survivor sets S_0 (after Phase A) ..
 * S_{k-1}, and the projected columns C fetched last:
 *
 *   seq   = sum over F u C of [ R(c) + D(c) + N*U ]
 *   staged = R(f_0) + D(f_0) + N*U                                    Phase A
 *         + sum_{i>=1} [ rho(S_{i-1})*R(f_i) + H(f_i,S_{i-1})*dpb(f_i) + |S_{i-1}|*U ]
 *         + sum over C of [ rho(S_{k-1})*R(c) + H(c,S_{k-1})*dpb(c) + |S_{k-1}|*U ]
 *
 * The k = 1 case is exactly the single-column model that was calibrated on
 * the benchmark; a projected column that is also a stage is fetched once, by
 * its stage, and is not counted in C.
 *
 * Two more terms belong to every Phase B fetch and were found on the
 * optimized build at TPC-H SF100, where the ordinary scan's own per-row
 * work is small enough for them to show: positioning inside a hit block
 * advances datum by datum (datumstreamread_find()), so a block holding s
 * survivors pays about rpb*s/(s+1) of the per-datum work the ordinary scan
 * pays for the whole block -- half a block for a lone survivor, all of it
 * once the block holds many -- and every block walked past or into costs a
 * header read, a content skip and cursor bookkeeping. With survivors in
 * nearly every block the first term approaches the ordinary scan's and the
 * second is pure overhead: measured, such predicates gain nothing (0.1% and
 * up scattered over lineitem) while sparse ones still gain 60% (0.01%) and
 * clustered ones 70-90%.
 *
 * Estimating H and rho
 * --------------------
 * Both hinge on how clustered the surviving rows are in row-number order,
 * which for an append-optimized table is insertion order -- exactly what
 * STATISTIC_KIND_CORRELATION measures. A survivor set is modelled as
 * scattered uniformly inside a *band* of the file: a clause on a column whose
 * |correlation| is 1 confines its matches to a band as wide as its
 * selectivity, one with no correlation confines them to nothing. Interpolating
 * on the square of the correlation, as cost_index() does for its own AO
 * decompression charge, a stage with selectivity sel on a column with
 * correlation-squared csq narrows the band by a factor
 *
 *   b = 1 - csq * (1 - sel)
 *
 * and the survivors of several stages live in the product of their bands.
 * Inside a band of fraction 'band' with S survivors:
 *
 *   H(c,S) = band*B(c) * (1 - exp(-S / (band*B(c))))
 *   rho(S) = 1 - (1 - band)/2
 *
 * That is, the distinct blocks hit by S random draws over the band's blocks,
 * saturating at the band; and a band at a uniformly random position ends,
 * on average, halfway between its own width and the end of the file. For a
 * single perfectly clustered clause H reduces to sel*B(c) and rho to
 * 1 - (1-sel)/2; for a scattered one to B(c)*(1-exp(-S/B(c))) and 1 -- the
 * two endpoints the single-column benchmark validated (H within a block or
 * two of the injector counts across the matrix, rho = 0.5 predicted, 0.499
 * measured).
 *
 * With no statistics correlation defaults to 0: survivors are modelled as
 * scattered, no block skipping is claimed, and Staged Scan has to justify
 * itself on the per-datum saving alone. Conservative in the right direction.
 *
 * Ordering the stages
 * -------------------
 * Selectivities along a candidate are estimated cumulatively -- all clauses
 * decided so far are handed to clauselist_selectivity() together -- so that
 * extended statistics (CREATE STATISTICS ... (dependencies, mcv)) and the
 * estimator's handling of several clauses on one column shape the order:
 * a clause implied by an earlier one is seen to reject nothing.
 *
 * A stage is worth its own cost plus what it saves every column after it, so
 * "most selective first" is not the rule: a narrow column with moderate
 * selectivity can beat a wide column with better selectivity as Phase A,
 * because Phase A pays a full pass. The (s - 1) / c rank ordering is optimal
 * only for independent predicates of constant cost (Kastrati & Moerkotte,
 * PVLDB 2016); here a stage's cost depends on how many rows and blocks the
 * stages before it let through, so candidates are costed end to end instead.
 *
 * The candidates are derived from the predicate's structure rather than by
 * permuting columns: a clause is decided only once the last of its columns
 * is in hand, so the meaningful choice is the order in which the *clauses*
 * are decided. Every order of up to STAGED_MAX_ENUM_CLAUSES clauses is
 * expanded into a column order -- each clause appends the columns it still
 * needs, an OR clause disjunct by disjunct with the disjuncts most likely to
 * settle the row per unit of cost first -- and costed; the first clause's
 * columns form Phase A. Beyond that many clauses only the increasing
 * selectivity order is tried. An OR clause placed later in the order can
 * still be settled early for a row by a disjunct whose columns happen to be
 * in hand (the executor's bypass); the cost of a column that only OR clauses
 * need is scaled by the fraction of rows on which some earlier disjunct of
 * every such clause has not already settled it.
 *
 * Note that skipping a block does NOT avoid reading its bytes -- the storage
 * layer still pulls them through the buffered-read layer (see
 * AppendOnlyStorageRead_SkipCurrentBlock). Only the (1-rho) tail is a genuine
 * I/O saving; everything else Staged Scan saves is CPU.
 *
 * Portions Copyright (c) 2026, WarehousePG contributors.
 *
 * IDENTIFICATION
 *	    src/backend/optimizer/path/stagedscancost.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/relation.h"
#include "access/sysattr.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_statistic.h"
#include "cdb/cdbaocsam_stagedscan.h"
#include "cdb/cdbappendonlyam.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/walkers.h"
#include "parser/parsetree.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/syscache.h"

/*
 * Per-datum unpack cost. Charged per column per row it is fetched for, which
 * is what datumstreamread_advance()/_get() actually cost; this is the
 * component Staged Scan saves even when it cannot skip a single block.
 */
#define STAGED_CPU_DATUM_COST	(cpu_operator_cost)

/*
 * Per-datum cost of positioning inside a hit varblock (see the header
 * comment). Calibrated equal to the unpack cost on the SF100 release-build
 * sweep (bench_scale.sql, group sel): stepping past a datum is the same
 * datumstreamread_advance() the ordinary scan does.
 */
#define STAGED_CPU_STEP_COST		(cpu_operator_cost)

/*
 * Phase B's per-block work on every block it walks past or into -- header
 * read and parse, content skip, cursor bookkeeping -- charged like one
 * decompression. Not a measured primitive: the fitted constant that puts the
 * accept/decline boundary where the same sweep measured it (survivors in
 * nearly every block: no gain; fewer than one per block: 60%).
 */
#define STAGED_BLOCK_WALK_COST	(gp_cpu_decompress_cost)

/* Every clause order is costed up to this many clauses (720 orders). */
#define STAGED_MAX_ENUM_CLAUSES	6

/* More filter columns than this and Staged Scan is not attempted at all. */
/*
 * An OR clause with more disjuncts than this is modelled as one clause,
 * decided where its last column arrives: expanding it disjunct by disjunct
 * costs O(disjuncts^2) per candidate order, times up to 720 orders, which
 * a long IN-like OR list would turn into hundreds of milliseconds of
 * planning. The executor still evaluates such a clause disjunct by disjunct
 * and bypasses the rest once one is true; only the cost model is coarser.
 */
#define STAGED_MAX_BYPASS_DISJUNCTS	8

#define STAGED_MAX_STAGES		8

/* What the model knows about one column of the relation. */
typedef struct StagedColInfo
{
	AttrNumber	attno;			/* one-based */
	double		nblocks;		/* B(c) */
	double		pages;			/* R(c), in pages */
	Cost		dpb;			/* cost to decompress one varblock */
	double		csq;			/* correlation squared, filter columns only */
	bool		projected;		/* in the scan's target list */
	bool		byval;			/* fixed-width pass-by-value type */
	bool		filter;			/* referenced by the predicate */
} StagedColInfo;

/*
 * One clause of the predicate and the columns it needs. An OR clause whose
 * disjuncts the executor can bypass also knows each disjunct's columns and
 * selectivity.
 */
typedef struct StagedClause
{
	RestrictInfo *rinfo;
	Bitmapset  *attnos;			/* one-based attnos it references */
	double		sel;
	int			ndisj;			/* 0 unless an OR clause evaluated by disjunct */
	Bitmapset **disjAttnos;
	double	   *disjSel;

	/*
	 * Settled by partition pruning: the decision is made once for every
	 * partition, on the partitioned parent (a GPORCA DynamicSeqScan), and
	 * the clause is true for every row of each partition it does not
	 * exclude. The parent's statistics may still call it selective; inside
	 * any partition the scan opens it keeps every row. It can be a later
	 * stage (the run-time reordering moves a stage that rejects nothing
	 * back) but never Phase A on its own.
	 */
	bool		pruned;

	/*
	 * Can fail on some row (stagedscan_clause_can_fail()): it must then not
	 * be decided before any clause ahead of it in the scan's qual order,
	 * which is the order of cx->clauses (see staged_order_cost()).
	 */
	bool		canFail;
} StagedClause;

typedef struct StagedCostContext
{
	PlannerInfo *root;
	RelOptInfo *rel;
	List	   *scanClauses;	/* the scan's clauses, in its qual order */
	double		ntuples;
	double		spc_seq_page_cost;
	StagedColInfo *cols;			/* indexed by attno - 1 */
	int			ncols;
	StagedClause *clauses;
	int			nclauses;

	/*
	 * Cumulative selectivity per set of clauses, indexed by the set's bitmask
	 * of clause indexes; < 0 until computed. Only when every clause order is
	 * enumerated: the orders share their prefixes, and a selectivity estimate
	 * with extended statistics deserializes them every time it is asked.
	 */
	double	   *selMemo;
} StagedCostContext;

/*
 * Cost of decompressing one varblock of column 'attno', following the same
 * codec weighting as get_ao_decompress_coefficient() but per column rather
 * than averaged over the whole relation.
 */
static Cost
staged_col_decompress_cost(RelOptInfo *rel, AttrNumber attno)
{
	AOStorageInfo *info;

	if (rel->ao_storage_info == NULL ||
		rel->ao_storage_info->col_storage_infos == NULL ||
		attno < 1 || attno > rel->max_attr)
		return 0.0;

	info = &rel->ao_storage_info->col_storage_infos[attno - 1];

	if (pg_strcasecmp(info->compresstype, "zstd") == 0)
		return gp_cpu_decompress_cost;
	if (pg_strcasecmp(info->compresstype, "zlib") == 0)
		return 2 * gp_cpu_decompress_cost;
	if (pg_strcasecmp(info->compresstype, "rle_type") == 0)
	{
		if (info->compresslevel == 1)
			return 0.0;						/* RLE alone, nothing to inflate */
		if (info->compresslevel >= 2 && info->compresslevel <= 4)
			return 2 * gp_cpu_decompress_cost;	/* zlib on top of RLE */
		return gp_cpu_decompress_cost;			/* zstd on top of RLE */
	}
	return 0.0;								/* uncompressed */
}

/*
 * |correlation| of a base relation column, or 0 when unknown.
 *
 * Zero is the conservative answer: it models the survivors as scattered, so
 * the estimate claims no block skipping at all and Staged Scan has to justify
 * itself on the per-datum saving alone.
 */
static double staged_col_correlation(Oid relid, AttrNumber attno, bool inh);

/*
 * Correlation of column 'attno' of a partitioned table, as the scan will meet
 * it: a Dynamic Seq Scan reads one partition after another and Phase B skips
 * blocks within a partition, so what matters is how clustered the column is
 * inside each leaf, not across the whole tree. The parent's inherited
 * statistics carry no correlation at all (ANALYZE computes none for the
 * merged sample), so take the leaves' own, weighted by their rows. Leaves
 * may order their columns differently; match them by name.
 */
static double
staged_partition_correlation(Oid parentOid, AttrNumber attno)
{
	char	   *attname = get_attname(parentOid, attno, true);
	List	   *children;
	ListCell   *lc;
	double		weighted = 0.0;
	double		tuples = 0.0;

	if (attname == NULL)
		return 0.0;
	children = find_all_inheritors(parentOid, NoLock, NULL);
	foreach(lc, children)
	{
		Oid			childOid = lfirst_oid(lc);
		HeapTuple	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(childOid));
		Form_pg_class form;

		if (!HeapTupleIsValid(tuple))
			continue;
		form = (Form_pg_class) GETSTRUCT(tuple);
		if (form->relkind == RELKIND_RELATION && form->reltuples > 0)
		{
			AttrNumber	childAttno = get_attnum(childOid, attname);

			if (childAttno != InvalidAttrNumber)
			{
				weighted += form->reltuples *
					staged_col_correlation(childOid, childAttno, false);
				tuples += form->reltuples;
			}
		}
		ReleaseSysCache(tuple);
	}
	list_free(children);
	pfree(attname);
	return tuples > 0.0 ? weighted / tuples : 0.0;
}

static double
staged_col_correlation(Oid relid, AttrNumber attno, bool inh)
{
	HeapTuple	tup;
	double		corr = 0.0;

	if (!OidIsValid(relid))
		return 0.0;
	if (inh)
		return staged_partition_correlation(relid, attno);

	tup = SearchSysCache3(STATRELATTINH,
						  ObjectIdGetDatum(relid),
						  Int16GetDatum(attno),
						  BoolGetDatum(false));
	if (HeapTupleIsValid(tup))
	{
		AttStatsSlot sslot;

		if (get_attstatsslot(&sslot, tup, STATISTIC_KIND_CORRELATION,
							 InvalidOid, ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers > 0)
				corr = fabs(sslot.numbers[0]);
			free_attstatsslot(&sslot);
		}
		ReleaseSysCache(tup);
	}

	return Min(corr, 1.0);
}

/* Is column 'attno' of 'relid' dropped? (a whole-row Var skips those) */
static bool
staged_att_is_dropped(Oid relid, AttrNumber attno)
{
	HeapTuple	tup;
	bool		dropped = true;

	tup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(attno));
	if (HeapTupleIsValid(tup))
	{
		dropped = ((Form_pg_attribute) GETSTRUCT(tup))->attisdropped;
		ReleaseSysCache(tup);
	}
	return dropped;
}

/*
 * pg_statistic's average width of a column, from the plain or the inherited
 * row as 'inh' says (get_attavgwidth() reads only the plain one, which a
 * partitioned parent does not have). 0 when there are no statistics.
 */
static int32
staged_col_stawidth(Oid relid, AttrNumber attno, bool inh)
{
	HeapTuple	tup;
	int32		width = 0;

	if (!OidIsValid(relid))
		return 0;
	tup = SearchSysCache3(STATRELATTINH,
						  ObjectIdGetDatum(relid),
						  Int16GetDatum(attno),
						  BoolGetDatum(inh));
	if (HeapTupleIsValid(tup))
	{
		width = ((Form_pg_statistic) GETSTRUCT(tup))->stawidth;
		ReleaseSysCache(tup);
	}
	return width;
}

/*
 * The varblock size of column 'attno', from its attribute encoding
 * (pg_attribute_encoding, gathered into rel->ao_storage_info by plancat),
 * or the default when the relation has no per-column options.
 */
static int32
staged_col_blocksize(RelOptInfo *rel, AttrNumber attno)
{
	if (rel->ao_storage_info != NULL &&
		rel->ao_storage_info->col_storage_infos != NULL &&
		attno >= 1 && attno <= rel->max_attr &&
		rel->ao_storage_info->col_storage_infos[attno - 1].blocksize > 0)
		return rel->ao_storage_info->col_storage_infos[attno - 1].blocksize;
	return DEFAULT_APPENDONLY_BLOCK_SIZE;
}

/*
 * Number of varblocks column 'attno' occupies, at its own block size.
 *
 * rel->attr_widths is only filled for columns the scan projects; a column
 * that appears in the predicate alone falls back to pg_statistic's average
 * width, then to the type's.
 */
static double
staged_col_varblocks(RelOptInfo *rel, Oid relid, bool inh,
					AttrNumber attno, double ntuples)
{
	int32		width = 0;

	if (attno >= rel->min_attr && attno <= rel->max_attr)
		width = rel->attr_widths[attno - rel->min_attr];
	if (width <= 0)
		width = staged_col_stawidth(relid, attno, inh);
	if (width <= 0)
	{
		Oid			typid;
		int32		typmod;
		Oid			collid;

		get_atttypetypmodcoll(relid, attno, &typid, &typmod, &collid);
		width = get_typavgwidth(typid, typmod);
	}
	if (width <= 0)
		width = 8;					/* no stats yet; assume a narrow column */

	return Max(1.0, (width * ntuples) / staged_col_blocksize(rel, attno));
}

/* Blocks of a column that hold at least one of 'nsurv' rows scattered over
 * the fraction 'band' of the file. */
static double
staged_hblocks(double nblocks, double band, double nsurv)
{
	double		bb = Max(band * nblocks, 1e-9);
	double		h;

	if (nsurv <= 0)
		return 0.0;
	h = bb * (1.0 - exp(-nsurv / bb));
	return Max(0.0, Min(h, nblocks));
}

/* Fraction of a segfile Phase B walks before the survivors run out. */
static double
staged_rho(double band)
{
	return 1.0 - (1.0 - band) / 2.0;
}

/*
 * Cost of fetching column 'c' in Phase B for 'nsurv' rows inside 'band'.
 *
 * 'justRead' is for a Phase A column Phase B has to walk again (a
 * pass-by-reference type, or a Phase A wider than one column; a single
 * by-value Phase A column is carried with the survivors and costs Phase B
 * nothing): Phase A has just pulled the whole file through the page cache,
 * so charging the I/O a second time is wrong -- and measurably so: it made
 * the model lead with a narrow 30% column ahead of a wide column keeping 13
 * rows, which ran 18% slower than the other way round. What the second walk
 * does pay is the decompression of the blocks that hold survivors, and the
 * datums.
 */
static Cost
staged_fetch_cost(StagedCostContext *cx, StagedColInfo *c, double band,
				 double nsurv, bool justRead)
{
	double		rho = staged_rho(band);
	double		h = staged_hblocks(c->nblocks, band, nsurv);
	double		rpb = cx->ntuples / c->nblocks;	/* rows per varblock */
	double		perblock = (h > 0.0) ? nsurv / h : 0.0;	/* survivors per hit block */
	double		stepped = h * rpb * (perblock / (perblock + 1.0));
	Cost		io = justRead ? 0.0 : rho * c->pages * cx->spc_seq_page_cost;

	return io
		+ h * c->dpb
		+ nsurv * STAGED_CPU_DATUM_COST
		+ stepped * STAGED_CPU_STEP_COST
		+ rho * c->nblocks * STAGED_BLOCK_WALK_COST;
}

/* Cost of reading column 'c' in full, as the ordinary scan and Phase A do. */
static Cost
staged_full_cost(StagedCostContext *cx, StagedColInfo *c)
{
	return c->pages * cx->spc_seq_page_cost
		+ c->nblocks * c->dpb
		+ cx->ntuples * STAGED_CPU_DATUM_COST;
}

/*
 * Stage at which a clause is decided under 'order': the last stage among the
 * columns it references. -1 if it references a column that is not a stage.
 */
static int
staged_clause_stage(StagedClause *cl, const AttrNumber *order, int nstages)
{
	int			stage = 0;
	int			attno = -1;

	while ((attno = bms_next_member(cl->attnos, attno)) >= 0)
	{
		int			s;

		for (s = 0; s < nstages; s++)
			if (order[s] == attno)
				break;
		if (s == nstages)
			return -1;
		if (s > stage)
			stage = s;
	}
	return stage;
}

/*
 * Position of 'attno' in 'order', or -1.
 */
static int
staged_col_pos(const AttrNumber *order, int nstages, int attno)
{
	for (int s = 0; s < nstages; s++)
		if (order[s] == attno)
			return s;
	return -1;
}

/*
 * Stage at which one disjunct of an OR clause can be evaluated under
 * 'order': the last stage among its columns, -1 if one is not a stage.
 */
static int
staged_disjunct_stage(const Bitmapset *attnos, const AttrNumber *order,
					 int nstages)
{
	int			stage = 0;
	int			attno = -1;

	while ((attno = bms_next_member(attnos, attno)) >= 0)
	{
		int			s = staged_col_pos(order, nstages, attno);

		if (s < 0)
			return -1;
		if (s > stage)
			stage = s;
	}
	return stage;
}

/*
 * The fraction of the rows reaching stage 'p' for which the executor fetches
 * the stage's column 'attno'. 1 when a projected column or a clause evaluated
 * as a whole needs it. When only OR clauses need it, the column is skipped
 * on a row once every one of them has been settled by an earlier true
 * disjunct, so the fraction is the largest, over those clauses, probability
 * that the clause is still open: the product of (1 - p_j) over the disjuncts
 * evaluable before this stage, assuming independence.
 */
static double
staged_fetch_fraction(StagedCostContext *cx, const AttrNumber *order,
					 int nstages, int p, int attno)
{
	double		frac = 0.0;
	bool		anyOr = false;

	if (cx->cols[attno - 1].projected)
		return 1.0;

	for (int i = 0; i < cx->nclauses; i++)
	{
		StagedClause *cl = &cx->clauses[i];
		double		open = 1.0;

		if (!bms_is_member(attno, cl->attnos))
			continue;
		if (cl->ndisj == 0)
			return 1.0;

		anyOr = true;
		for (int j = 0; j < cl->ndisj; j++)
		{
			int			e = staged_disjunct_stage(cl->disjAttnos[j], order, nstages);

			if (e >= 0 && e < p)
				open *= 1.0 - cl->disjSel[j];
		}
		frac = Max(frac, open);
	}
	return anyOr ? frac : 1.0;
}

/*
 * Selectivity of the clauses in 'decided' together; 'mask' is their set as a
 * bitmask of clause indexes, used only when cx->selMemo exists.
 */
static double
staged_cum_selectivity(StagedCostContext *cx, List *decided, uint32 mask)
{
	double		sel;

	if (cx->selMemo != NULL && cx->selMemo[mask] >= 0.0)
		return cx->selMemo[mask];
	sel = clauselist_selectivity(cx->root, decided, cx->rel->relid,
								 JOIN_INNER, NULL);
	if (cx->selMemo != NULL)
		cx->selMemo[mask] = sel;
	return sel;
}

/*
 * Cost one stage ordering whose first 'k0' columns are read in full by Phase
 * A. Returns the total Staged Scan cost, or -1 if the ordering is unusable
 * because Phase A would have no clause to apply (it would keep every row).
 *
 * An ordering that leaves nothing to fetch after Phase A -- the target list
 * names only Phase A's columns, or nothing at all, as count(*) does -- is a
 * valid candidate. Its cost comes out equal to the ordinary scan's, since the
 * model charges both the same for reading the filter column; the tie is
 * settled in Staged Scan's favour by the caller, see staged_enumerate_orders().
 *
 * k0 is normally 1. It grows only for a clause that needs several columns
 * before it can drop a row -- d = X OR e = Y, or a + b > 10 -- since every
 * column added to Phase A costs a full pass, which a later stage never does.
 */
static Cost
staged_order_cost(StagedCostContext *cx, const AttrNumber *order, int nstages,
				 int k0, double *phaseASel)
{
	double		band = 1.0;
	double		nsurv = cx->ntuples;
	double		csq = 1.0;
	double		sel;
	double		cumsel;			/* selectivity of every clause decided so far */
	Cost		total = 0.0;
	List	   *clauses = NIL;
	List	   *decided = NIL;	/* those clauses, for the cumulative estimate */
	uint32		mask = 0;		/* and as a set, for cx->selMemo */
	bool		phaseAFilters = false;	/* a Phase A clause pruning left open */
	int			latest = 0;

	/*
	 * A clause that can fail is decided no earlier than every clause before
	 * it in the qual: the rule stagedscan_applicable() enforces at run time,
	 * on the same stage numbering (the Phase A columns are stage 0).
	 */
	for (int i = 0; i < cx->nclauses; i++)
	{
		int			pos = staged_clause_stage(&cx->clauses[i], order, nstages);
		int			stage = pos < k0 ? 0 : pos - k0 + 1;

		if (cx->clauses[i].canFail && stage < latest)
			return -1;
		latest = Max(latest, stage);
	}

	/* Phase A: the clauses confined to its columns, evaluated on a full pass. */
	for (int i = 0; i < cx->nclauses; i++)
	{
		int			pos = staged_clause_stage(&cx->clauses[i], order, nstages);

		if (pos >= 0 && pos < k0)
		{
			clauses = lappend(clauses, cx->clauses[i].rinfo);
			if (cx->selMemo != NULL)
				mask |= (uint32) 1 << i;
			if (!cx->clauses[i].pruned)
				phaseAFilters = true;
		}
	}
	if (clauses == NIL || !phaseAFilters)
		return -1;

	for (int p = 0; p < k0; p++)
	{
		StagedColInfo *c = &cx->cols[order[p] - 1];

		total += staged_full_cost(cx, c);
		/* the most scattered Phase A column bounds how narrow the band gets */
		csq = Min(csq, c->csq);
	}

	/*
	 * Selectivities are estimated cumulatively: the clauses decided up to and
	 * including a stage are handed to clauselist_selectivity() together, and
	 * the stage's own selectivity is the ratio to the previous total. Given
	 * the clauses together, the estimator can apply extended statistics
	 * (functional dependencies, MCV lists) and its own handling of several
	 * clauses on one column, so a stage whose clause is implied by an earlier
	 * one is seen to reject nothing and is costed -- and ordered --
	 * accordingly. Handing each stage's clauses over alone would multiply the
	 * stages as if independent, which is exactly the assumption stale or
	 * missing statistics get wrong.
	 */
	decided = list_copy(clauses);
	list_free(clauses);
	cumsel = staged_cum_selectivity(cx, decided, mask);
	sel = cumsel;
	/*
	 * The executor compares what Phase A keeps against this estimate and
	 * falls back when the two are far apart; an estimate of zero (a NULL
	 * test against a null_frac of 0, say) would disarm that check just
	 * when it is most needed, so it is at least one row.
	 */
	if (phaseASel != NULL)
		*phaseASel = Max(cumsel, 1.0 / Max(cx->ntuples, 1.0));
	band *= 1.0 - csq * (1.0 - sel);
	nsurv = cx->ntuples * cumsel;

	/* Phase B stages: one column each, fetched for the survivors so far. */
	for (int p = k0; p < nstages; p++)
	{
		StagedColInfo *c = &cx->cols[order[p] - 1];
		double		newcum;

		clauses = NIL;
		for (int i = 0; i < cx->nclauses; i++)
		{
			if (staged_clause_stage(&cx->clauses[i], order, nstages) == p)
			{
				clauses = lappend(clauses, cx->clauses[i].rinfo);
				if (cx->selMemo != NULL)
					mask |= (uint32) 1 << i;
			}
		}

		total += staged_fetch_cost(cx, c, band,
								  nsurv * staged_fetch_fraction(cx, order, nstages,
															   p, c->attno),
								  false);

		if (clauses == NIL)
			sel = 1.0;
		else
		{
			/* list_concat() links clauses' cells into decided */
			decided = list_concat(decided, clauses);
			newcum = staged_cum_selectivity(cx, decided, mask);
			sel = cumsel > 0.0 ? Min(1.0, newcum / cumsel) : 1.0;
			cumsel = Min(cumsel, newcum);
		}

		band *= 1.0 - c->csq * (1.0 - sel);
		nsurv = cx->ntuples * cumsel;
	}
	list_free(decided);

	/* the projected columns no stage fetched, against the final survivors */
	for (int i = 0; i < cx->ncols; i++)
	{
		StagedColInfo *c = &cx->cols[i];
		bool		staged = false;
		bool		inPhaseA = false;

		if (!c->projected)
			continue;
		for (int p = 0; p < nstages; p++)
		{
			if (order[p] != c->attno)
				continue;
			if (p < k0)
				inPhaseA = true;
			else
				staged = true;
		}
		if (staged)
			continue;
		/*
		 * A single by-value Phase A column is carried with the survivors
		 * (StagedScanState.carryAttno): Phase B never walks it. Any
		 * other Phase A column is walked again, without I/O.
		 */
		if (inPhaseA && k0 == 1 && c->byval)
			continue;
		total += staged_fetch_cost(cx, c, band, nsurv, inPhaseA);
	}

	return total;
}

/* Lexicographic next permutation of perm[0..n-1]; false once exhausted. */
static bool
staged_next_permutation(int *perm, int n)
{
	int			i = n - 2;
	int			j;

	while (i >= 0 && perm[i] >= perm[i + 1])
		i--;
	if (i < 0)
		return false;
	j = n - 1;
	while (perm[j] <= perm[i])
		j--;
	{
		int			t = perm[i];

		perm[i] = perm[j];
		perm[j] = t;
	}
	for (int lo = i + 1, hi = n - 1; lo < hi; lo++, hi--)
	{
		int			t = perm[lo];

		perm[lo] = perm[hi];
		perm[hi] = t;
	}
	return true;
}

/* Append to order[] the members of 'attnos' not yet in it. */
static void
staged_append_columns(const Bitmapset *attnos, AttrNumber *order, int *n)
{
	int			attno = -1;

	while ((attno = bms_next_member(attnos, attno)) >= 0)
		if (staged_col_pos(order, *n, attno) < 0)
			order[(*n)++] = attno;
}

/*
 * Cost of reading the columns of 'attnos' that are not yet in order[], in
 * full; the yardstick for choosing which disjunct of an OR clause to try
 * first.
 */
static Cost
staged_unplaced_cost(StagedCostContext *cx, const Bitmapset *attnos,
					const AttrNumber *order, int n)
{
	Cost		cost = 0.0;
	int			attno = -1;

	while ((attno = bms_next_member(attnos, attno)) >= 0)
		if (staged_col_pos(order, n, attno) < 0)
			cost += staged_full_cost(cx, &cx->cols[attno - 1]);
	return cost;
}

/*
 * Expand a clause order into a column order: each clause in turn appends the
 * columns it still needs. An OR clause appends them disjunct by disjunct,
 * the disjunct with the highest chance of settling the row per unit of
 * column cost first, so that the executor's bypass has the most to gain.
 *
 * Returns the number of columns, and sets *k0 to the width of the first
 * clause: the Phase A columns.
 */
static int
staged_expand_clause_order(StagedCostContext *cx, const int *perm, int nperm,
						  AttrNumber *order, int *k0)
{
	int			n = 0;

	*k0 = 0;
	for (int c = 0; c < nperm; c++)
	{
		StagedClause *cl = &cx->clauses[perm[c]];

		/*
		 * The first clause is Phase A, which reads all of its columns in
		 * lockstep: their order does not matter there, so keep it plain.
		 */
		if (cl->ndisj == 0 || c == 0)
			staged_append_columns(cl->attnos, order, &n);
		else
		{
			bool	   *placed = (bool *) palloc0(sizeof(bool) * cl->ndisj);

			for (int round = 0; round < cl->ndisj; round++)
			{
				int			bestj = -1;
				double		bestScore = -1.0;

				for (int j = 0; j < cl->ndisj; j++)
				{
					Cost		cost;
					double		score;

					if (placed[j])
						continue;
					cost = staged_unplaced_cost(cx, cl->disjAttnos[j], order, n);
					/* nothing to add: free, and first */
					score = cost <= 0.0 ? DBL_MAX : cl->disjSel[j] / cost;
					if (score > bestScore)
					{
						bestScore = score;
						bestj = j;
					}
				}
				placed[bestj] = true;
				staged_append_columns(cl->disjAttnos[bestj], order, &n);
			}
			pfree(placed);
		}
		if (c == 0)
			*k0 = n;
	}
	return n;
}

/* Order clause indexes by increasing selectivity, ties by index. */
static void
staged_sort_clauses_by_selectivity(StagedCostContext *cx, int *perm, int n)
{
	for (int a = 1; a < n; a++)
		for (int b = a;
			 b > 0 && cx->clauses[perm[b - 1]].sel > cx->clauses[perm[b]].sel;
			 b--)
		{
			int			t = perm[b - 1];

			perm[b - 1] = perm[b];
			perm[b] = t;
		}
}

/*
 * Collect the predicate's columns, and each clause's.
 *
 * Fills cx->clauses with the clauses the executor will actually see, each
 * with the columns it references, its selectivity and -- for an OR clause the
 * executor evaluates disjunct by disjunct -- the same per disjunct; the union
 * of the referenced columns comes back in *qualattrs.
 *
 * Returns false when the predicate cannot be staged at all: nothing left once
 * the pseudoconstant clauses are dropped, an ordering a row-level-security
 * policy imposes, or a clause the stage ExprContext could not evaluate.
 */
static bool
staged_collect_clauses(StagedCostContext *cx, Bitmapset **qualattrs)
{
	PlannerInfo *root = cx->root;
	RelOptInfo *rel = cx->rel;
	List	   *scanclauses;
	ListCell   *lc;
	int			i;

	/*
	 * Row-level security orders the quals: order_qual_clauses() keeps a clause
	 * behind every clause of a lower security level unless it is leakproof,
	 * so a user's function never sees a row the policy hides. Staged Scan
	 * evaluates the stages in its own order, and the executor may reorder
	 * them again at run time, so when the predicate mixes security levels and
	 * one of the guarded clauses could leak, leave the scan alone.
	 */
	{
		Index		minlevel = 0;
		bool		first = true;

		foreach(lc, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			if (first || rinfo->security_level < minlevel)
				minlevel = rinfo->security_level;
			first = false;
		}
		foreach(lc, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			if (rinfo->security_level > minlevel && !rinfo->leakproof)
				return false;
		}
	}

	/*
	 * A pseudoconstant clause is not part of the scan's qual (create_plan()
	 * moves it into a gating Result above the scan), so the executor never
	 * sees it; costing it here would credit Phase A with rows it does not
	 * reject. The rest are kept in the scan's qual order.
	 */
	scanclauses = NIL;
	foreach(lc, cx->scanClauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (!rinfo->pseudoconstant)
			scanclauses = lappend(scanclauses, rinfo);
	}
	if (scanclauses == NIL)
		return false;
	cx->nclauses = list_length(scanclauses);
	cx->clauses = (StagedClause *) palloc0(sizeof(StagedClause) * cx->nclauses);

	i = 0;
	foreach(lc, scanclauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Bitmapset  *attrs = NULL;
		int			bit = -1;

		/*
		 * The executor evaluates the stage quals in a standalone ExprContext
		 * that has no parameter values, so a qual with a Param -- a generic
		 * plan's $n, or the exec-param a nested loop would feed a
		 * parameterized scan -- cannot be staged. (Ordinary Var-on-Const
		 * clauses under a custom plan are fine.)
		 */
		if (stagedscan_contains_param((Node *) rinfo->clause) ||
			contain_volatile_functions((Node *) rinfo->clause) ||
			contain_subplans((Node *) rinfo->clause))
			return false;

		pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs);
		cx->clauses[i].rinfo = rinfo;
		cx->clauses[i].attnos = NULL;
		while ((bit = bms_next_member(attrs, bit)) >= 0)
		{
			int			a = bit + FirstLowInvalidHeapAttributeNumber;

			/* system columns and whole-row references cannot be staged */
			if (a <= 0)
				return false;
			cx->clauses[i].attnos = bms_add_member(cx->clauses[i].attnos, a);
			*qualattrs = bms_add_member(*qualattrs, a);
		}
		cx->clauses[i].sel = clause_selectivity(root, (Node *) rinfo,
												rel->relid, JOIN_INNER,
												NULL, false);
		cx->clauses[i].canFail = stagedscan_clause_can_fail((Node *) rinfo->clause);

		/*
		 * An OR clause the executor evaluates disjunct by disjunct (unless
		 * it can fail; see stagedscan_split_or()): note each disjunct's
		 * columns and selectivity, for the expansion order and the
		 * bypass-aware fetch fraction.
		 */
		cx->clauses[i].ndisj = 0;
		cx->clauses[i].disjAttnos = NULL;
		cx->clauses[i].disjSel = NULL;
		if (is_orclause(rinfo->clause) &&
			!cx->clauses[i].canFail &&
			list_length(((BoolExpr *) rinfo->clause)->args) <= STAGED_MAX_BYPASS_DISJUNCTS)
		{
			List	   *args = ((BoolExpr *) rinfo->clause)->args;
			int			nd = list_length(args);
			int			j = 0;
			ListCell   *dlc;

			cx->clauses[i].ndisj = nd;
			cx->clauses[i].disjAttnos = (Bitmapset **) palloc(sizeof(Bitmapset *) * nd);
			cx->clauses[i].disjSel = (double *) palloc(sizeof(double) * nd);
			foreach(dlc, args)
			{
				Node	   *disj = (Node *) lfirst(dlc);
				Bitmapset  *dattrs = NULL;
				int			dbit = -1;

				cx->clauses[i].disjAttnos[j] = NULL;
				pull_varattnos(disj, rel->relid, &dattrs);
				while ((dbit = bms_next_member(dattrs, dbit)) >= 0)
					cx->clauses[i].disjAttnos[j] =
						bms_add_member(cx->clauses[i].disjAttnos[j],
									   dbit + FirstLowInvalidHeapAttributeNumber);
				cx->clauses[i].disjSel[j] =
					clause_selectivity(root, disj, rel->relid, JOIN_INNER,
									   NULL, false);
				j++;
			}
		}
		i++;
	}

	return true;
}

/*
 * The per-column facts.
 *
 * Fills cx->cols for every column the scan touches -- its varblock count and
 * pages, what one of its varblocks costs to decompress, whether the predicate
 * or the target list wants it and, for a filter column, how clustered it is --
 * and returns what an ordinary sequential scan pays to read all of them.
 */
static Cost
staged_collect_columns(StagedCostContext *cx, Oid relid, bool inh,
					  Bitmapset *qualattrs)
{
	RelOptInfo *rel = cx->rel;
	Bitmapset  *projattrs = NULL;
	Cost		seqcost = 0.0;
	int			attno;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &projattrs);
	/* a whole-row reference fetches every live column */
	if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, projattrs))
	{
		for (attno = 1; attno <= rel->max_attr; attno++)
			if (!staged_att_is_dropped(relid, attno))
				projattrs = bms_add_member(projattrs,
										   attno - FirstLowInvalidHeapAttributeNumber);
	}

	cx->ncols = rel->max_attr;
	cx->cols = (StagedColInfo *) palloc0(sizeof(StagedColInfo) * cx->ncols);
	get_tablespace_page_costs(rel->reltablespace, NULL, &cx->spc_seq_page_cost);

	for (attno = 1; attno <= rel->max_attr; attno++)
	{
		StagedColInfo *c = &cx->cols[attno - 1];
		int			bit = attno - FirstLowInvalidHeapAttributeNumber;

		c->attno = attno;
		c->projected = bms_is_member(bit, projattrs);
		c->filter = bms_is_member(attno, qualattrs);
		if (!c->projected && !c->filter)
			continue;

		c->nblocks = staged_col_varblocks(rel, relid, inh, attno, cx->ntuples);
		c->pages = (c->nblocks * staged_col_blocksize(rel, attno)) / BLCKSZ;
		c->dpb = staged_col_decompress_cost(rel, attno);
		{
			Oid			typid;
			int32		typmod;
			Oid			collid;
			int16		typlen;

			get_atttypetypmodcoll(relid, attno, &typid, &typmod, &collid);
			get_typlenbyval(typid, &typlen, &c->byval);
			c->byval = c->byval && typlen > 0;
		}
		if (c->filter)
		{
			double		corr = staged_col_correlation(relid, attno, inh);

			c->csq = corr * corr;
		}

		/* the ordinary scan reads every one of these in full */
		seqcost += staged_full_cost(cx, c);
	}

	return seqcost;
}

/*
 * Cost every candidate order and keep the cheapest one that beats 'seqcost'.
 *
 * Candidates are clause orders, each expanded into the column order that
 * decides the clauses in that sequence, with the first clause's columns as
 * Phase A. Every clause order up to STAGED_MAX_ENUM_CLAUSES clauses; beyond
 * that, increasing selectivity only.
 */
static bool
staged_enumerate_orders(StagedCostContext *cx, const AttrNumber *filters,
					   int nfilters, Cost seqcost, AttrNumber *bestorder,
					   int *bestn, int *bestk0, double *bestsel)
{
	AttrNumber	order[STAGED_MAX_STAGES];
	Cost		best = -1.0;
	int		   *perm = (int *) palloc(sizeof(int) * cx->nclauses);
	bool		exhaustive = (cx->nclauses <= STAGED_MAX_ENUM_CLAUSES);

	memcpy(order, filters, sizeof(AttrNumber) * nfilters);

	for (int i = 0; i < cx->nclauses; i++)
		perm[i] = i;
	if (!exhaustive)
		staged_sort_clauses_by_selectivity(cx, perm, cx->nclauses);

	do
	{
		int			nstages;
		int			k0;
		Cost		cost;
		double		sel = 0.0;

		nstages = staged_expand_clause_order(cx, perm, cx->nclauses,
											order, &k0);
		/*
		 * Every filter column, once; anything else is a bug, but one that
		 * should cost the query its Staged Scan, not the query itself.
		 */
		Assert(nstages == nfilters);
		if (nstages != nfilters)
		{
			pfree(perm);
			return false;
		}
		cost = staged_order_cost(cx, order, nstages, k0, &sel);
		if (cost >= 0 && (best < 0 || cost < best))
		{
			best = cost;
			memcpy(bestorder, order, sizeof(AttrNumber) * nstages);
			*bestn = nstages;
			*bestk0 = k0;
			*bestsel = sel;
		}
	} while (exhaustive && staged_next_permutation(perm, cx->nclauses));
	pfree(perm);

	/*
	 * The block-skipping term rests on STATISTIC_KIND_CORRELATION,
	 * which is blind to a common physical layout: a table loaded by
	 * several parallel sessions holds several interleaved ascending
	 * runs per segment, its correlation is near 0, and yet a band of
	 * key values sits in a few blocks per run (a 1% l_orderkey band on
	 * a gpfdist-loaded TPC-H lineitem: correlation -0.09, 70% faster
	 * with Staged Scan, declined by the model above). A single-stage
	 * predicate the pessimistic estimate declines is therefore
	 * re-costed as if its column were perfectly clustered; when that
	 * says yes the executor gets it and decides on the evidence: the
	 * first batch of survivors shows how many blocks they really
	 * occupy, and Phase A falls back to the ordinary scan before any
	 * row is emitted if they are dense (stagedscan_run_phase_a()).
	 * Only a single stage can fall back, which is why the retry is
	 * limited to one filter column.
	 */
	if ((best < 0 || best > seqcost) && nfilters == 1)
	{
		StagedColInfo *c = &cx->cols[filters[0] - 1];
		double		csq = c->csq;
		double		sel = 0.0;
		Cost		cost;

		c->csq = 1.0;
		cost = staged_order_cost(cx, filters, 1, 1, &sel);
		c->csq = csq;
		if (cost >= 0 && cost < seqcost)
		{
			best = cost;
			bestorder[0] = filters[0];
			*bestn = 1;
			*bestk0 = 1;
			*bestsel = sel;
		}
	}
	/*
	 * A tie goes to the Staged Scan. The model charges neither scan for
	 * forming a tuple -- cpu_tuple_cost-like work the ordinary scan pays for
	 * every row and the Staged Scan only for the survivors, since the rows a
	 * stage drops never reach a slot -- so where the two come out equal, as
	 * they do when nothing is left to fetch after Phase A (count(*), or a
	 * target list that names only the filter column), the Staged Scan is the
	 * cheaper one by exactly that omitted term. Measured on a 30M-row table:
	 * count(*) on an integer predicate 4.5x faster through Phase A's fast
	 * path, a text LIKE a third faster, and a predicate dense enough to fall
	 * back after the first batch no slower than the ordinary scan.
	 */
	if (best < 0 || best > seqcost)
		return false;
	return true;
}

/*
 * Is 'clause' settled by partition pruning: refuted by, or implied by, the
 * bound of every partition of 'prel'? Only the top level is looked at; a
 * clause on a sub-partition key is left open, which only costs it the
 * chance to be recognised.
 */
static bool
staged_settled_by_pruning(Relation prel, Index varno, Expr *clause)
{
	PartitionDesc pdesc = RelationGetPartitionDesc(prel);
	List	   *pred = list_make1(clause);

	for (int i = 0; i < pdesc->nparts; i++)
	{
		HeapTuple	tuple;
		Datum		boundDatum;
		bool		isnull;
		List	   *bound = NIL;

		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(pdesc->oids[i]));
		if (!HeapTupleIsValid(tuple))
			return false;
		boundDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relpartbound,
									 &isnull);
		if (!isnull)
			bound = get_qual_from_partbound(NULL, prel,
											castNode(PartitionBoundSpec,
													 stringToNode(TextDatumGetCString(boundDatum))));
		ReleaseSysCache(tuple);

		/* the bound's Vars are the parent's, as varno 1 */
		if (varno != 1)
			ChangeVarNodes((Node *) bound, 1, varno, 0);
		/*
		 * Weak refutation is what pruning means here: no row of the
		 * partition makes the clause true, whether it makes it false or
		 * NULL. Implication has to be strong: every row keeps it.
		 */
		if (!predicate_refuted_by(pred, bound, true) &&
			!predicate_implied_by(pred, bound, false))
			return false;
	}
	return true;
}

/*
 * Mark the clauses partition pruning settles (StagedClause.pruned), for a
 * decision made on the partitioned parent 'relid'. Only clauses on the
 * partition key's columns alone can be.
 */
static void
staged_mark_pruned_clauses(StagedCostContext *cx, Oid relid)
{
	Relation	prel = relation_open(relid, NoLock);
	PartitionKey key = RelationGetPartitionKey(prel);
	Bitmapset  *keyAttnos = NULL;

	for (int i = 0; i < key->partnatts; i++)
		if (key->partattrs[i] != 0)
			keyAttnos = bms_add_member(keyAttnos, key->partattrs[i]);

	for (int i = 0; i < cx->nclauses; i++)
	{
		StagedClause *cl = &cx->clauses[i];

		if (keyAttnos != NULL && bms_is_subset(cl->attnos, keyAttnos))
			cl->pruned = staged_settled_by_pruning(prel, cx->rel->relid,
												   cl->rinfo->clause);
	}
	relation_close(prel, NoLock);
}

/*
 * Decide whether Staged Scan is worth it for this relation, and how to stage
 * its predicate.
 *
 * Returns true and sets *filterAttnos (an IntList of one-based attnos, stage
 * order) and *phaseAWidth (how many leading entries Phase A reads in full)
 * when the two-phase scan is estimated to be cheaper than an ordinary AOCO
 * sequential scan. 'clauses' are rel's restrictions as RestrictInfos in the
 * order the scan's qual will evaluate them.
 *
 * This is permission, not instruction: the executor re-checks the structural
 * preconditions it alone can see (missing-value filter column, the scan type)
 * and silently declines if any fail.
 */
bool
cost_aocs_stagedscan(PlannerInfo *root, RelOptInfo *rel, List *clauses,
					 List **filterAttnos, int *phaseAWidth, double *phaseASel)
{
	StagedCostContext cx;
	Bitmapset  *qualattrs = NULL;
	Oid			relid;
	bool		inh;
	AttrNumber	filters[STAGED_MAX_STAGES];
	int			nfilters = 0;
	double		ntuples,
				nsurvivors,
				sel;
	Cost		seqcost;
	AttrNumber	bestorder[STAGED_MAX_STAGES];
	int			bestn = 0;
	int			bestk0 = 0;
	int			attno;
	bool		chosen = false;
	List	   *scanquals = NIL;
	bool		pseudo = false;
	ListCell   *lc;
	MemoryContext costcxt;
	MemoryContext oldcxt;

	*filterAttnos = NIL;
	*phaseAWidth = 0;
	*phaseASel = 0.0;

	if (!whpg_enable_staged_scan)
		return false;
	if (!IsAccessMethodAOColumn(rel->relam))
		return false;
	if (rel->baserestrictinfo == NIL)
		return false;

	ntuples = rel->tuples;
	nsurvivors = rel->rows;
	if (ntuples <= 0 || nsurvivors < 0)
		return false;

	sel = Min(1.0, nsurvivors / ntuples);

	/*
	 * rel->rows counts the pseudoconstant clauses too, which a gating Result
	 * evaluates once and the scan never sees; the gate is about the scan's
	 * own predicate.
	 */
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->pseudoconstant)
			pseudo = true;
		else
			scanquals = lappend(scanquals, rinfo);
	}
	if (pseudo)
		sel = clauselist_selectivity(root, scanquals, rel->relid,
									 JOIN_INNER, NULL);
	list_free(scanquals);

	/*
	 * Above whpg_staged_scan_min_selectivity the executor abandons Phase A
	 * partway through and restarts as an ordinary scan, which measures 5-18%
	 * *slower* than never having tried. Decline up front instead: this is the
	 * whole reason the decision belongs in the planner rather than in
	 * aoco_beginscan_extractcolumns(), which cannot see a selectivity estimate.
	 *
	 * For a multi-column predicate this is the selectivity of the whole
	 * predicate; Phase A alone may keep more, and the model below decides
	 * whether the later stages earn that back.
	 */
	if (sel > whpg_staged_scan_min_selectivity)
		return false;

	relid = planner_rt_fetch(rel->relid, root)->relid;
	/* a partitioned parent is costed on its inherited statistics */
	inh = planner_rt_fetch(rel->relid, root)->inh;

	cx.root = root;
	cx.rel = rel;
	cx.scanClauses = clauses;
	cx.ntuples = ntuples;
	cx.selMemo = NULL;

	/*
	 * Everything the decision allocates -- the clause and column tables, and
	 * whatever the selectivity estimator builds, extended statistics
	 * included -- goes away with it: the Postgres planner decides once per
	 * partition of a partitioned scan.
	 */
	costcxt = AllocSetContextCreate(CurrentMemoryContext,
									"Staged Scan costing",
									ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(costcxt);

	if (staged_collect_clauses(&cx, &qualattrs))
	{
		bool		usable = true;

		if (inh && get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
			staged_mark_pruned_clauses(&cx, relid);

		attno = -1;
		while ((attno = bms_next_member(qualattrs, attno)) >= 0)
		{
			if (nfilters >= STAGED_MAX_STAGES || attno > rel->max_attr)
			{
				usable = false;
				break;
			}
			filters[nfilters++] = attno;
		}

		if (usable && nfilters > 0)
		{
			if (cx.nclauses <= STAGED_MAX_ENUM_CLAUSES)
			{
				int			nsets = 1 << cx.nclauses;

				cx.selMemo = (double *) palloc(sizeof(double) * nsets);
				for (int i = 0; i < nsets; i++)
					cx.selMemo[i] = -1.0;
			}

			seqcost = staged_collect_columns(&cx, relid, inh, qualattrs);

			/*
			 * staged_order_cost() returns -1 for a candidate with no clause
			 * Phase A could apply, so such candidates drop out of the
			 * enumeration.
			 */
			chosen = staged_enumerate_orders(&cx, filters, nfilters, seqcost,
											 bestorder, &bestn, &bestk0,
											 phaseASel);
		}
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(costcxt);

	if (!chosen)
		return false;

	for (int s = 0; s < bestn; s++)
		*filterAttnos = lappend_int(*filterAttnos, bestorder[s]);
	*phaseAWidth = bestk0;

	return true;
}
