/*-------------------------------------------------------------------------
 *
 * cdbaocsam_stagedscan.h
 *	  Staged Scan: two-phase scan for AOCO tables.
 *
 * Staged Scan is not an index. It is an executor-level optimization for
 * highly selective predicates on AOCO tables. Instead of advancing every
 * projected column's datum stream in lockstep and evaluating the qual
 * afterwards (which pays the decompression cost for rows that are about to
 * be thrown away), it splits the scan in two:
 *
 *	Phase A	 scans only the leading filter column's segfile -- or, for a
 *			 clause that needs several columns before it can drop a row
 *			 (d = X OR e = Y), those columns in lockstep -- evaluates the
 *			 clauses confined to them row by row, consults the visimap, and
 *			 records the row numbers that survive.
 *
 *	Phase B	 walks each remaining column's segfile by varblock *header*, and
 *			 decompresses a block's content only when the block's row range
 *			 intersects the surviving row numbers. Blocks holding no survivor
 *			 are stepped over without being decompressed.
 *
 * A predicate on several columns is handled by *staging* Phase B: the filter
 * columns after the first are fetched, one row at a time, before the
 * projected columns, and each one's clauses are evaluated as soon as its
 * value is in hand. A row rejected at stage i never asks stage i+1 or the
 * projected columns for anything, so every later column skips blocks against
 * a survivor set that shrinks at each stage. No separate pass per column and
 * no survivor-array rewriting is needed for that; it falls out of the order
 * in which one row's columns are fetched.
 *
 * See src/backend/access/aocs/README.staged_scan.md for the full design.
 *
 * Portions Copyright (c) 2026, WarehousePG contributors.
 *
 * IDENTIFICATION
 *	    src/include/cdb/cdbaocsam_stagedscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBAOCSAM_STAGEDSCAN_H
#define CDBAOCSAM_STAGEDSCAN_H

#include "access/attnum.h"
#include "cdb/cdbappendonlyblockdirectory.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "utils/palloc.h"

struct AOCSScanDescData;
struct AOCSFileSegInfo;
struct DatumStreamRead;

/* GUCs -- see guc_gp.c */
extern bool whpg_enable_staged_scan;
extern double whpg_staged_scan_min_selectivity;

/*
 * The row numbers that survived Phase A, ascending: one batch of a segfile.
 *
 * Phase A produces row numbers in increasing order by construction, so no
 * sort is needed and Phase B can intersect this against varblock row ranges
 * with a single forward cursor. Phase A pauses once the batch holds
 * a batch of survivors (STAGEDSCAN_BATCH_MAX_ROWS at most, see
 * aocs_stagedscan.c); Phase B consumes them and Phase A
 * resumes where it stopped, so the memory is bounded whatever the segfile's
 * size and hit count. Every column's Phase B cursor only moves forward, and
 * successive batches are ascending too, so batches are invisible to Phase B.
 */
typedef struct StagedScanSurvivors
{
	int64	   *rownums;
	int64		nrows;
	int64		capacity;

	/*
	 * The Phase A column's datum for each survivor, when Phase A is one
	 * by-value column (StagedScanState.carryAttno): Phase A has the
	 * value in hand when it records the row, and Phase B would otherwise
	 * walk that column's segfile a second time whenever the target list or
	 * a later stage asks for it. NULL when nothing is carried.
	 */
	Datum	   *values;
	bool	   *isnull;
} StagedScanSurvivors;

/*
 * Per-column Phase B cursor.
 *
 * 'contentLoaded' distinguishes "the header of the current block has been
 * read" from "the content of the current block has been decompressed", which
 * is exactly the distinction datumstreamread_block_header_only() /
 * _block_content() / _block_skip_content() expose.
 */
typedef struct StagedScanColumnCursor
{
	bool		exhausted;		/* no more blocks in this column's segfile */
	bool		headerValid;	/* a header has been read and not yet consumed */
	bool		contentLoaded;	/* current block's content is decompressed */
	int64		blockFirstRowNum;
	int64		blockLastRowNum;
	int64		blockAfterOffset;	/* file offset just past the current block */
	int64		blockBytes;		/* on-disk length of the current block */

	/*
	 * Directory seeks that cannot help: none past the directory's last entry
	 * (dirExhausted, for the rest of the segfile), and no second try for a
	 * row whose entry led nowhere ahead (noSeekRow; row numbers start at 1).
	 */
	bool		dirExhausted;
	int64		noSeekRow;
} StagedScanColumnCursor;

/*
 * One top-level AND clause of the predicate, as Phase B sees it.
 *
 * A clause is decided at the stage where the last of its columns arrives.
 * An OR clause is additionally evaluated one disjunct at a time as soon as
 * that disjunct's columns are in the slot: a true disjunct settles the clause
 * early (bypass), and the columns that only its later disjuncts needed are
 * not fetched for that row.
 */
typedef struct StagedScanClause
{
	Node	   *expr;
	int			decideStage;	/* stage at which the whole clause is settled */
	bool		isOr;			/* evaluated disjunct by disjunct */
	int			ndisj;
} StagedScanClause;

/* One evaluation a stage performs: a whole clause, or one disjunct of an OR. */
typedef struct StagedScanEval
{
	int			clause;
	int			disjunct;		/* -1 for the whole clause */
	bool		last;			/* last disjunct: an undecided OR is rejected here */
	ExprState  *qual;
#ifdef USE_ASSERT_CHECKING
	bool	   *cols;			/* the columns it reads, indexed by attno */
#endif
} StagedScanEval;

/*
 * One Phase B stage.
 *
 * The columns in 'fetchAttnos' are fetched for the row (block-skipping
 * against the rows that reached this stage), then 'evals' run in order and
 * the first rejection abandons the row. 'fetchAttnos' is normally just
 * 'attno'; it also carries a Phase A column when a clause decided here needs
 * it, because Phase A's datums do not outlive Phase A. A column needed only
 * by OR clauses is skipped for a row on which all of them are already
 * settled ('fetchNeededBy' lists those clauses; NULL means always fetch).
 */
typedef struct StagedScanStage
{
	AttrNumber	attno;			/* zero-based column this stage is about */
	AttrNumber *fetchAttnos;
	List	  **fetchNeededBy;	/* per fetchAttno: IntList of OR clauses, or NULL */
	int			nfetch;
	StagedScanEval *evals;
	int			nevals;

	/* per-segment observations, for the adaptive reordering */
	int64		rowsIn;
	int64		rowsRejected;
	int64		rowsSettled;	/* an OR clause settled here by a true disjunct */
	int64		blocksDecompressed;
} StagedScanStage;

typedef struct StagedScanSimpleClause
{
	int			strategy;		/* BTLessStrategyNumber .. BTGreaterStrategyNumber */
	int64		value;			/* the constant, widened to int64 */
} StagedScanSimpleClause;

typedef struct StagedScanState
{
	/*
	 * The stage order: 'stageAttnos[0..npos)' with the first 'nphaseA' read by
	 * Phase A; stages[0] stands for Phase A, stages[1..nstages) for the
	 * remaining positions. Rebuilt in 'stageCtx' when the adaptive reordering
	 * changes the Phase B order at a segment boundary.
	 */
	AttrNumber *stageAttnos;
	int			npos;
	List	   *qual;
	StagedScanStage *stages;
	int			nstages;
	ExprState  *phaseAQual;		/* the clauses confined to the Phase A columns */

	/*
	 * Phase A fast path. When Phase A is one fixed-width column and every
	 * clause on it is <column op constant> with a btree comparison operator of
	 * the column's type family, the clauses are compiled into this array and a
	 * block whose datums lie contiguous (Original format, no NULLs) is tested
	 * with a C loop over the raw datum array instead of one
	 * datumstreamread_advance() + ExecQual() per row. 'nsimple' is 0 when the
	 * predicate does not qualify; blocks that do not qualify (NULLs, RLE) go
	 * the row-at-a-time way regardless.
	 */
	struct StagedScanSimpleClause *simple;
	int			nsimple;
	int			simpleLen;		/* attlen of the Phase A column: 2, 4 or 8 */
	int64		blocksFastA;	/* blocks Phase A tested through the fast path */
	StagedScanClause *clauses;
	int			nclauses;
	bool	   *clauseDecided;	/* per row, indexed by clause */
	MemoryContext stageCtx;

	/*
	 * Per clause of 'qual': can it fail on some row (stagedscan_clause_can_fail())?
	 * Such a clause is shown only the rows the ordinary scan would show it:
	 * the stage order keeps it behind the clauses before it, and Phase A
	 * checks the visimap before evaluating one ('phaseAMayFail').
	 */
	bool	   *clauseCanFail;
	bool		phaseAMayFail;

#ifdef USE_ASSERT_CHECKING
	/*
	 * Per column, the survivor whose value the filter slot and the output
	 * slot last received: every evaluation must read only columns written
	 * for the current survivor, and every row emitted must carry every
	 * column its consumers read.
	 */
	int64		rowStamp;
	int64	   *filterStamp;
	int64	   *outStamp;
#endif

	/* projected columns fetched once every stage has passed */
	AttrNumber *restAttnos;
	int			nrest;

	ExprContext *econtext;
	TupleTableSlot *filterSlot; /* virtual slot the stage quals are evaluated on */

	/*
	 * Phase A reads its columns through its own streams rather than the scan's
	 * columnScanInfo.ds[], so that a column can still be materialized normally
	 * by Phase B when it also appears in the target list or in a later stage's
	 * clause. The segfile is read twice in that case, but Phase B only
	 * decompresses the blocks that hold survivors.
	 *
	 * Normally one column. Several only when a clause needs them all before it
	 * can drop a row (d = X OR e = Y); they are then advanced in lockstep, row
	 * by row, exactly as aocs_getnext() advances the projected columns.
	 */
	AttrNumber *phaseAAttnos;	/* zero-based; the first 'nphaseA' stage columns */
	int			nphaseA;
	double		phaseASel;		/* the planner's estimate of what Phase A keeps, 0 = unknown */
	struct DatumStreamRead **filterDs;	/* one per Phase A column */
	bool		filterDsOpen;

	StagedScanSurvivors surv;
	int64		cursor;			/* Phase B position within surv.rownums */
	int64		curSurv;		/* the survivor Phase B is materializing */
	AttrNumber	carryAttno;		/* zero-based Phase A column carried in surv, or -1 */
	bool		phaseADone;		/* a batch is ready for Phase B (or the segfile is exhausted) */
	bool		phaseAEof;		/* Phase A has read this segfile to its end */
	int64		phaseABatches;	/* batches produced for this segfile so far */
	int64		batchTarget;	/* survivors the current batch fills up to */
	int64		rowsScannedSeg; /* Phase A rows scanned in this segfile */
	bool		fellBack;		/* Phase A gave up before any row was emitted */

	/*
	 * How the survivors of the first batch are spread over the anchor Phase A
	 * column's varblocks: blocks entered so far in this segfile, blocks that
	 * held at least one survivor, and the block the cursor is in. The first
	 * batch's density decides whether Phase B has anything to skip (see
	 * stagedscan_run_phase_a()).
	 */
	int64		blocksScannedA;
	int64		blocksHitA;
	int64		curBlockFirstRow;
	bool		curBlockHit;

	StagedScanColumnCursor *colCursor;	/* indexed by attno */
	AttrNumber	ncols;

	/*
	 * The table's block directory, opened for reading, or NULL when the table
	 * has none (it is built with the table's first index). Phase B uses it to
	 * seek a column's stream straight to the varblock holding the next
	 * survivor when that block is not the next one in the file: the header
	 * walk would still pull every skipped block's bytes through the buffered
	 * reader, a seek reads none of them. Without it Phase B walks headers.
	 */
	AppendOnlyBlockDirectory *blkdir;

	/* observability */
	int64		rowsScanned;
	int64		rowsSurviving;
	int64		rowsInvisible;
	int64		rowsInvisibleSeg;	/* of those, in the current segfile */
	int64		segHidden;		/* deleted rows of the current segfile (visimap) */
	/*
	 * Deleted rows that failed the predicate. Phase A evaluates the predicate
	 * before it consults the visimap, so they are counted as scanned but
	 * never as invisible; an ordinary scan never shows them to its filter, so
	 * "Rows Removed by Filter" must not count them either. Settled per
	 * segfile from the visimap's hidden-row count.
	 */
	int64		rowsHiddenRejected;
	int64		rowsRejectedLater;	/* Phase A survivors a later stage rejected */
	int64		rowsBypassed;		/* rows an early true disjunct settled */
	int64		blocksSkipped;
	int64		blocksSeeked;		/* seeks through the block directory */
	int64		blocksJumped;		/* blocks those seeks passed over without reading; counted from the directory, only when instrumented */
	int64		blocksDecompressed;
	int64		stageReorders;

	/*
	 * Adaptive reordering windows: survivors processed since the last
	 * re-scoring, and the size of the current window (doubles after each
	 * check). Reset per segfile.
	 */
	int64		rowsSinceAdapt;
	int64		adaptWindow;

	MemoryContext ctx;
} StagedScanState;

extern bool extractcolumns_from_node(Node *expr, bool *cols, AttrNumber natts);
extern bool stagedscan_contains_param(Node *node);
extern bool stagedscan_clause_can_fail(Node *clause);
extern List *stagedscan_flatten_qual(List *qual);

extern bool stagedscan_applicable(Relation rel, List *targetlist, List *qual,
								  bool *proj, AttrNumber natts,
								  List *filterAttnos, int phaseAWidth);

extern StagedScanState *stagedscan_begin(struct AOCSScanDescData *scan,
											 List *filterAttnos,
											 int phaseAWidth,
											 List *qual,
											 ExprContext *econtext,
											 MemoryContext ctx);
extern void stagedscan_end(StagedScanState *gi);
extern void stagedscan_reset_for_segment(StagedScanState *gi);
extern void stagedscan_adapt_stage_order(StagedScanState *gi,
										 struct AOCSScanDescData *scan);
extern bool stagedscan_run_phase_a(StagedScanState *gi,
								   struct AOCSScanDescData *scan,
								   struct AOCSFileSegInfo *segInfo);

#endif							/* CDBAOCSAM_STAGEDSCAN_H */
