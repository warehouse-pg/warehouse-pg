/*-------------------------------------------------------------------------
 *
 * nodeWindowTopN.c
 *	  Keep the top-N rows of each window partition, by rank.
 *
 * This node is the top-N pre-filter of a window's input.  It is placed
 * below the Sort, and the Motion if any, that bring a window's partitions
 * together, on a query of the form
 *
 *		SELECT ... FROM (SELECT ..., rank() OVER (PARTITION BY p ORDER BY k)
 *						 AS rk FROM ...) s WHERE rk <= N
 *
 * and keeps, for each PARTITION BY group of its own input, only the rows
 * whose window function value would satisfy the filter when computed over
 * that input alone.  A row's rank over a subset of its partition is never
 * larger than its rank over the whole partition, so every row dropped here
 * would also have been dropped by the outer query's filter.
 *
 * The node is only an optimization: the final WindowAgg above recomputes
 * the function and the outer query still applies the filter.  It therefore
 * does not spill.  When the rows it holds exceed its memory budget it
 * returns everything held so far and passes the remaining input through
 * unfiltered, which is still correct.
 *
 * Data structure
 *
 * Partitions are the entries of a TupleHashTable keyed on the PARTITION BY
 * columns (or one static group when there is no PARTITION BY).  Each
 * partition holds a max-heap of buckets ordered by the ORDER BY columns,
 * so that heap[0] is the bucket with the largest, i.e. worst-ranked, key.
 * A bucket holds all kept rows that share one sort key.  The rules for
 * admitting a row and for pruning the heap depend on the window function:
 *
 *	row_number: every row is its own bucket; at most N rows are kept.
 *	rank:		rows with equal keys share a bucket; the rows of the worst
 *				bucket have rank 1 + (rows in the other buckets), so a bucket
 *				is dropped once the other buckets alone hold N rows.
 *	dense_rank: as rank, but the bound applies to the number of buckets.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeWindowTopN.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeWindowTopN.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/sortsupport.h"

/* a kept row */
typedef struct WindowTopNRow
{
	MinimalTuple tuple;
	struct WindowTopNRow *next;	/* next row of the same bucket */
} WindowTopNRow;

/* all kept rows of one partition that share a sort key */
typedef struct WindowTopNBucket
{
	WindowTopNRow *rows;		/* the first one is the representative */
	int			nrows;
	Datum	   *keys;			/* sort key of the representative; by-ref */
	bool	   *nulls;			/* values point into its tuple */
} WindowTopNBucket;

/* one partition */
typedef struct WindowTopNGroup
{
	WindowTopNBucket **heap;	/* max-heap by sort key; heap[0] is the worst */
	int			nbuckets;
	int			capacity;
	int64		nrows;			/* rows in all buckets */
} WindowTopNGroup;

#define INITIAL_HEAP_CAPACITY	16

static TupleTableSlot *ExecWindowTopN(PlanState *pstate);
static void compute_limit(WindowTopNState *node);
static WindowTopNGroup *lookup_group(WindowTopNState *node, TupleTableSlot *slot);
static void consider_row(WindowTopNState *node, WindowTopNGroup *group,
						 TupleTableSlot *slot);
static int	compare_keys(WindowTopNState *node, const Datum *keys1, const bool *nulls1,
						 const Datum *keys2, const bool *nulls2);
static void extract_keys(WindowTopNState *node, TupleTableSlot *slot,
						 Datum *keys, bool *nulls);
static WindowTopNBucket *make_bucket(WindowTopNState *node, TupleTableSlot *slot);
static void append_row(WindowTopNState *node, WindowTopNGroup *group,
					   WindowTopNBucket *bucket, TupleTableSlot *slot);
static void free_bucket(WindowTopNState *node, WindowTopNBucket *bucket);
static void heap_push(WindowTopNState *node, WindowTopNGroup *group,
					  WindowTopNBucket *bucket);
static void heap_pop(WindowTopNState *node, WindowTopNGroup *group);
static void heap_siftdown(WindowTopNState *node, WindowTopNGroup *group, int i);
static WindowTopNBucket *find_bucket(WindowTopNState *node, WindowTopNGroup *group,
									 const Datum *keys, const bool *nulls);
static bool group_has_room(WindowTopNState *node, WindowTopNGroup *group);
static void prune_group(WindowTopNState *node, WindowTopNGroup *group);
static void begin_drain(WindowTopNState *node, bool passthrough);
static TupleTableSlot *drain_next(WindowTopNState *node);
static void release_all(WindowTopNState *node);


/* ----------------------------------------------------------------
 *		ExecWindowTopN
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecWindowTopN(PlanState *pstate)
{
	WindowTopNState *node = castNode(WindowTopNState, pstate);
	PlanState  *outerPlan = outerPlanState(node);

	CHECK_FOR_INTERRUPTS();

	if (!node->limit_valid)
	{
		compute_limit(node);

		/*
		 * Nothing can pass a bound of zero or less (or a NULL bound: the
		 * outer query's operator is strict and filters every row).  Tell the
		 * input we are not going to read it.
		 */
		if (node->limit <= 0)
		{
			node->status = WINDOWTOPN_DONE;
			ExecSquelchNode(outerPlan);
		}
	}

	for (;;)
	{
		TupleTableSlot *slot;

		switch (node->status)
		{
			case WINDOWTOPN_FILLING:
				slot = ExecProcNode(outerPlan);
				if (TupIsNull(slot))
				{
					begin_drain(node, false);
					break;
				}

				/*
				 * The hash table's equality function, like nodeAgg's, works
				 * on minimal tuple slots, so present the row as one.  The
				 * copy also serves as the stored row if the row is kept.
				 */
				ExecCopySlot(node->keyslot, slot);
				MemoryContextReset(node->tempcxt);
				consider_row(node, lookup_group(node, node->keyslot), node->keyslot);

				/*
				 * Out of memory: hand back what we have, then pass the rest
				 * of the input through.  The rows we hold are all rows the
				 * final stage needs, and so is everything that still comes.
				 */
				if (node->mem_used > node->mem_limit)
					begin_drain(node, true);
				break;

			case WINDOWTOPN_DRAINING:
				slot = drain_next(node);
				if (!TupIsNull(slot))
					return slot;
				release_all(node);
				node->status = node->passthrough ? WINDOWTOPN_PASSTHROUGH
					: WINDOWTOPN_DONE;
				break;

			case WINDOWTOPN_PASSTHROUGH:
				slot = ExecProcNode(outerPlan);
				if (TupIsNull(slot))
				{
					node->status = WINDOWTOPN_DONE;
					break;
				}
				/* our callers expect our own (minimal tuple) result slot */
				return ExecCopySlot(node->ss.ps.ps_ResultTupleSlot, slot);

			case WINDOWTOPN_DONE:
				return NULL;
		}
	}
}

/*
 * Evaluate the bound once per scan, like nodeLimit does for LIMIT.
 */
static void
compute_limit(WindowTopNState *node)
{
	WindowTopN *plan = (WindowTopN *) node->ss.ps.plan;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	Datum		val;
	bool		isnull;

	val = ExecEvalExprSwitchContext(node->limitExpr, econtext, &isnull);
	if (isnull)
		node->limit = 0;
	else
	{
		switch (exprType(plan->limitExpr))
		{
			case INT2OID:
				node->limit = DatumGetInt16(val);
				break;
			case INT4OID:
				node->limit = DatumGetInt32(val);
				break;
			case INT8OID:
				node->limit = DatumGetInt64(val);
				break;
			default:
				elog(ERROR, "unexpected type %u of WindowTopN limit",
					 exprType(plan->limitExpr));
		}
		if (!plan->limitInclusive)
			node->limit--;
	}
	node->limit_valid = true;
}

/*
 * Find or create the partition the input row belongs to.
 */
static WindowTopNGroup *
lookup_group(WindowTopNState *node, TupleTableSlot *slot)
{
	WindowTopNGroup *group;

	if (node->hashtable == NULL)
	{
		if (node->single_group == NULL)
		{
			node->single_group = MemoryContextAllocZero(node->tablecxt,
														sizeof(WindowTopNGroup));
			node->mem_used += sizeof(WindowTopNGroup);
		}
		return (WindowTopNGroup *) node->single_group;
	}
	else
	{
		TupleHashEntry entry;
		bool		isnew;

		entry = LookupTupleHashEntry(node->hashtable, slot, &isnew, NULL);
		if (isnew)
		{
			/* 'additional' is ours to allocate, as in nodeAgg.c */
			entry->additional = MemoryContextAllocZero(node->tablecxt,
													   sizeof(WindowTopNGroup));
			/* the entry and its copy of the grouping columns */
			node->mem_used += sizeof(TupleHashEntryData) + sizeof(WindowTopNGroup) +
				GetMemoryChunkSpace(entry->firstTuple);
		}
		group = (WindowTopNGroup *) entry->additional;
		return group;
	}
}

/*
 * Decide whether the input row stays, and keep the partition's heap within
 * its bound afterwards.
 */
static void
consider_row(WindowTopNState *node, WindowTopNGroup *group, TupleTableSlot *slot)
{
	WindowTopN *plan = (WindowTopN *) node->ss.ps.plan;
	WindowTopNBucket *top;
	int			cmp;

	extract_keys(node, slot, node->inkeys, node->innulls);

	if (group->nbuckets == 0)
	{
		heap_push(node, group, make_bucket(node, slot));
		return;
	}

	top = group->heap[0];
	cmp = compare_keys(node, node->inkeys, node->innulls, top->keys, top->nulls);

	if (cmp == 0 && plan->kind != WINDOWTOPN_ROW_NUMBER)
	{
		/*
		 * Same key as the worst kept bucket: the row has the same rank as
		 * those rows, which is within the bound, so it is always kept.
		 */
		append_row(node, group, top, slot);
		return;
	}

	if (cmp >= 0)
	{
		/*
		 * Worse than everything kept (or equal, for row_number).  Its rank is
		 * one more than what is held, so it stays only while there is room.
		 */
		if (group_has_room(node, group))
			heap_push(node, group, make_bucket(node, slot));
		return;
	}

	/* Better than the worst kept bucket: it is kept, something else may go. */
	if (plan->kind != WINDOWTOPN_ROW_NUMBER)
	{
		WindowTopNBucket *bucket = find_bucket(node, group, node->inkeys, node->innulls);

		if (bucket != NULL)
		{
			append_row(node, group, bucket, slot);
			prune_group(node, group);
			return;
		}
	}
	heap_push(node, group, make_bucket(node, slot));
	prune_group(node, group);
}

/*
 * Can a row ranked just after everything held still be within the bound?
 */
static bool
group_has_room(WindowTopNState *node, WindowTopNGroup *group)
{
	WindowTopN *plan = (WindowTopN *) node->ss.ps.plan;

	if (plan->kind == WINDOWTOPN_DENSE_RANK)
		return group->nbuckets < node->limit;
	return group->nrows < node->limit;
}

/*
 * Drop worst buckets whose rows can no longer be within the bound.
 */
static void
prune_group(WindowTopNState *node, WindowTopNGroup *group)
{
	WindowTopN *plan = (WindowTopN *) node->ss.ps.plan;

	switch (plan->kind)
	{
		case WINDOWTOPN_ROW_NUMBER:
			while (group->nrows > node->limit)
				heap_pop(node, group);
			break;
		case WINDOWTOPN_RANK:
			/* rank of the worst bucket's rows = 1 + rows in the other buckets */
			while (group->nbuckets > 0 &&
				   group->nrows - group->heap[0]->nrows >= node->limit)
				heap_pop(node, group);
			break;
		case WINDOWTOPN_DENSE_RANK:
			while (group->nbuckets > node->limit)
				heap_pop(node, group);
			break;
	}
}

static int
compare_keys(WindowTopNState *node, const Datum *keys1, const bool *nulls1,
			 const Datum *keys2, const bool *nulls2)
{
	WindowTopN *plan = (WindowTopN *) node->ss.ps.plan;
	int			i;

	for (i = 0; i < plan->ordNumCols; i++)
	{
		int			cmp = ApplySortComparator(keys1[i], nulls1[i],
											  keys2[i], nulls2[i],
											  &node->sortKeys[i]);

		if (cmp != 0)
			return cmp;
	}
	return 0;
}

static void
extract_keys(WindowTopNState *node, TupleTableSlot *slot, Datum *keys, bool *nulls)
{
	WindowTopN *plan = (WindowTopN *) node->ss.ps.plan;
	int			i;

	for (i = 0; i < plan->ordNumCols; i++)
		keys[i] = slot_getattr(slot, plan->ordColIdx[i], &nulls[i]);
}

/*
 * Copy the input row into tablecxt and return it as a WindowTopNRow.
 */
static WindowTopNRow *
copy_row(WindowTopNState *node, TupleTableSlot *slot)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(node->tablecxt);
	WindowTopNRow *row = palloc(sizeof(WindowTopNRow));

	row->tuple = ExecCopySlotMinimalTuple(slot);
	row->next = NULL;
	MemoryContextSwitchTo(oldcxt);

	node->mem_used += sizeof(WindowTopNRow) + GetMemoryChunkSpace(row->tuple);
	return row;
}

static WindowTopNBucket *
make_bucket(WindowTopNState *node, TupleTableSlot *slot)
{
	WindowTopN *plan = (WindowTopN *) node->ss.ps.plan;
	WindowTopNBucket *bucket;
	WindowTopNRow *row = copy_row(node, slot);

	bucket = MemoryContextAlloc(node->tablecxt, sizeof(WindowTopNBucket));
	bucket->rows = row;
	bucket->nrows = 1;
	bucket->keys = MemoryContextAlloc(node->tablecxt,
									  plan->ordNumCols * sizeof(Datum));
	bucket->nulls = MemoryContextAlloc(node->tablecxt,
									   plan->ordNumCols * sizeof(bool));

	/* re-extract the key from the copy, so by-ref values stay valid */
	ExecStoreMinimalTuple(row->tuple, node->keyslot, false);
	extract_keys(node, node->keyslot, bucket->keys, bucket->nulls);

	node->mem_used += sizeof(WindowTopNBucket) +
		plan->ordNumCols * (sizeof(Datum) + sizeof(bool));
	return bucket;
}

static void
append_row(WindowTopNState *node, WindowTopNGroup *group,
		   WindowTopNBucket *bucket, TupleTableSlot *slot)
{
	WindowTopNRow *row = copy_row(node, slot);

	row->next = bucket->rows->next;
	bucket->rows->next = row;
	bucket->nrows++;
	group->nrows++;
}

static void
free_bucket(WindowTopNState *node, WindowTopNBucket *bucket)
{
	WindowTopN *plan = (WindowTopN *) node->ss.ps.plan;
	WindowTopNRow *row = bucket->rows;

	while (row != NULL)
	{
		WindowTopNRow *next = row->next;

		node->mem_used -= sizeof(WindowTopNRow) + GetMemoryChunkSpace(row->tuple);
		pfree(row->tuple);
		pfree(row);
		row = next;
	}
	node->mem_used -= sizeof(WindowTopNBucket) +
		plan->ordNumCols * (sizeof(Datum) + sizeof(bool));
	pfree(bucket->keys);
	pfree(bucket->nulls);
	pfree(bucket);
}

/*
 * Linear search for a bucket with this key.  Only reached for rows that
 * rank better than the worst kept bucket, which is rare once a partition's
 * heap has settled, and N is small.
 */
static WindowTopNBucket *
find_bucket(WindowTopNState *node, WindowTopNGroup *group,
			const Datum *keys, const bool *nulls)
{
	int			i;

	for (i = 0; i < group->nbuckets; i++)
	{
		WindowTopNBucket *bucket = group->heap[i];

		if (compare_keys(node, keys, nulls, bucket->keys, bucket->nulls) == 0)
			return bucket;
	}
	return NULL;
}

/* max-heap: a parent never compares less than its children */
#define HEAP_LESS(node, a, b) \
	(compare_keys((node), (a)->keys, (a)->nulls, (b)->keys, (b)->nulls) < 0)

static void
heap_push(WindowTopNState *node, WindowTopNGroup *group, WindowTopNBucket *bucket)
{
	int			i;

	if (group->nbuckets >= group->capacity)
	{
		int			newcap = group->capacity ? group->capacity * 2 : INITIAL_HEAP_CAPACITY;
		MemoryContext oldcxt = MemoryContextSwitchTo(node->tablecxt);

		if (group->heap == NULL)
			group->heap = palloc(newcap * sizeof(WindowTopNBucket *));
		else
			group->heap = repalloc(group->heap, newcap * sizeof(WindowTopNBucket *));
		MemoryContextSwitchTo(oldcxt);
		node->mem_used += (newcap - group->capacity) * sizeof(WindowTopNBucket *);
		group->capacity = newcap;
	}

	/* sift up */
	i = group->nbuckets++;
	while (i > 0)
	{
		int			parent = (i - 1) / 2;

		if (!HEAP_LESS(node, group->heap[parent], bucket))
			break;
		group->heap[i] = group->heap[parent];
		i = parent;
	}
	group->heap[i] = bucket;
	group->nrows += bucket->nrows;
}

static void
heap_pop(WindowTopNState *node, WindowTopNGroup *group)
{
	WindowTopNBucket *top = group->heap[0];

	group->nrows -= top->nrows;
	free_bucket(node, top);

	group->nbuckets--;
	if (group->nbuckets > 0)
	{
		group->heap[0] = group->heap[group->nbuckets];
		heap_siftdown(node, group, 0);
	}
}

static void
heap_siftdown(WindowTopNState *node, WindowTopNGroup *group, int i)
{
	WindowTopNBucket *bucket = group->heap[i];

	for (;;)
	{
		int			left = 2 * i + 1;
		int			right = left + 1;
		int			largest = i;
		WindowTopNBucket *cand = bucket;

		if (left < group->nbuckets && HEAP_LESS(node, cand, group->heap[left]))
		{
			largest = left;
			cand = group->heap[left];
		}
		if (right < group->nbuckets && HEAP_LESS(node, cand, group->heap[right]))
		{
			largest = right;
			cand = group->heap[right];
		}
		if (largest == i)
			break;
		group->heap[i] = cand;
		i = largest;
	}
	group->heap[i] = bucket;
}

/*
 * Draining: walk partitions, buckets and rows.
 */
static void
begin_drain(WindowTopNState *node, bool passthrough)
{
	node->status = WINDOWTOPN_DRAINING;
	node->passthrough = passthrough;
	node->drain_group = NULL;
	node->drain_bucket = 0;
	node->drain_row = NULL;
	if (node->hashtable != NULL)
	{
		InitTupleHashIterator(node->hashtable, &node->hashiter);
		node->hashiter_valid = true;
	}
	else
		node->drain_group = node->single_group;
}

static TupleTableSlot *
drain_next(WindowTopNState *node)
{
	TupleTableSlot *slot = node->ss.ps.ps_ResultTupleSlot;

	for (;;)
	{
		WindowTopNGroup *group = (WindowTopNGroup *) node->drain_group;
		WindowTopNRow *row = (WindowTopNRow *) node->drain_row;

		if (row != NULL)
		{
			node->drain_row = row->next;
			ExecStoreMinimalTuple(row->tuple, slot, false);
			return slot;
		}

		/* next bucket of this group */
		if (group != NULL && node->drain_bucket < group->nbuckets)
		{
			node->drain_row = group->heap[node->drain_bucket]->rows;
			node->drain_bucket++;
			continue;
		}

		/* next group; without a hash table there is only the one */
		if (node->hashtable == NULL)
			return NULL;
		else
		{
			TupleHashEntry entry;

			if (!node->hashiter_valid)
				return NULL;
			entry = ScanTupleHashTable(node->hashtable, &node->hashiter);
			if (entry == NULL)
			{
				TermTupleHashIterator(&node->hashiter);
				node->hashiter_valid = false;
				return NULL;
			}
			node->drain_group = entry->additional;
			node->drain_bucket = 0;
		}
	}
}

/*
 * Drop everything held.  The hash table itself is rebuilt on rescan.
 */
static void
release_all(WindowTopNState *node)
{
	if (node->hashiter_valid)
	{
		TermTupleHashIterator(&node->hashiter);
		node->hashiter_valid = false;
	}
	MemoryContextReset(node->tablecxt);
	node->hashtable = NULL;
	node->single_group = NULL;
	node->mem_used = 0;
	node->drain_group = NULL;
	node->drain_row = NULL;
}

static void
build_hash_table(WindowTopNState *node)
{
	WindowTopN *plan = (WindowTopN *) node->ss.ps.plan;

	if (plan->partNumCols == 0)
	{
		node->hashtable = NULL;
		return;
	}

	node->hashtable = BuildTupleHashTableExt(&node->ss.ps,
											 ExecGetResultType(outerPlanState(node)),
											 plan->partNumCols,
											 plan->partColIdx,
											 node->eqfuncoids,
											 node->hashfunctions,
											 plan->partCollations,
											 256,
											 sizeof(WindowTopNGroup),
											 node->tablecxt,
											 node->tablecxt,
											 node->tempcxt,
											 false);
}

/* ----------------------------------------------------------------
 *		ExecInitWindowTopN
 * ----------------------------------------------------------------
 */
WindowTopNState *
ExecInitWindowTopN(WindowTopN *node, EState *estate, int eflags)
{
	WindowTopNState *state;
	Plan	   *outerPlan;
	TupleDesc	outerDesc;
	uint64		operatorMemKB;
	int			i;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	state = makeNode(WindowTopNState);
	state->ss.ps.plan = (Plan *) node;
	state->ss.ps.state = estate;
	state->ss.ps.ExecProcNode = ExecWindowTopN;
	state->status = WINDOWTOPN_FILLING;

	ExecAssignExprContext(estate, &state->ss.ps);

	state->tablecxt = AllocSetContextCreate(CurrentMemoryContext,
											"WindowTopN rows",
											ALLOCSET_DEFAULT_SIZES);
	state->tempcxt = AllocSetContextCreate(CurrentMemoryContext,
										   "WindowTopN hash lookup",
										   ALLOCSET_DEFAULT_SIZES);

	/*
	 * initialize child nodes
	 */
	outerPlan = outerPlan(node);
	outerPlanState(state) = ExecInitNode(outerPlan, estate, eflags);
	outerDesc = ExecGetResultType(outerPlanState(state));

	/*
	 * Initialize result slot and type.  The node returns its input rows
	 * unchanged, as minimal tuples.
	 */
	ExecInitResultTupleSlotTL(&state->ss.ps, &TTSOpsMinimalTuple);
	ExecInitScanTupleSlot(estate, &state->ss, outerDesc, &TTSOpsMinimalTuple);
	state->keyslot = state->ss.ss_ScanTupleSlot;
	state->ss.ps.ps_ProjInfo = NULL;

	state->limitExpr = ExecInitExpr((Expr *) node->limitExpr, (PlanState *) state);
	state->limit_valid = false;

	if (node->partNumCols > 0)
		execTuplesHashPrepare(node->partNumCols, node->partOperators,
							  &state->eqfuncoids, &state->hashfunctions);

	state->sortKeys = palloc0(node->ordNumCols * sizeof(SortSupportData));
	for (i = 0; i < node->ordNumCols; i++)
	{
		SortSupport sortKey = &state->sortKeys[i];

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = node->ordCollations[i];
		sortKey->ssup_nulls_first = node->nullsFirst[i];
		sortKey->ssup_attno = node->ordColIdx[i];
		PrepareSortSupportFromOrderingOp(node->sortOperators[i], sortKey);
	}
	state->inkeys = palloc(node->ordNumCols * sizeof(Datum));
	state->innulls = palloc(node->ordNumCols * sizeof(bool));

	operatorMemKB = PlanStateOperatorMemKB((PlanState *) state);
	if (operatorMemKB == 0)
		operatorMemKB = work_mem;
	state->mem_limit = operatorMemKB * 1024L;
	state->mem_used = 0;

	build_hash_table(state);

	return state;
}

/* ----------------------------------------------------------------
 *		ExecEndWindowTopN
 * ----------------------------------------------------------------
 */
void
ExecEndWindowTopN(WindowTopNState *node)
{
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecFreeExprContext(&node->ss.ps);
	MemoryContextDelete(node->tablecxt);
	MemoryContextDelete(node->tempcxt);
	ExecEndNode(outerPlanState(node));
}

/* ----------------------------------------------------------------
 *		ExecReScanWindowTopN
 * ----------------------------------------------------------------
 */
void
ExecReScanWindowTopN(WindowTopNState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	release_all(node);
	build_hash_table(node);
	node->status = WINDOWTOPN_FILLING;
	node->passthrough = false;
	node->limit_valid = false;	/* the bound may depend on a changed Param */

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

void
ExecSquelchWindowTopN(WindowTopNState *node)
{
	ExecSquelchNode(outerPlanState(node));
}
