/*-------------------------------------------------------------------------
 *
 * appendonlytid.c
 *
 * Portions Copyright (c) 2007-2009, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/backend/access/appendonly/appendonlytid.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/appendonlytid.h"
#include "optimizer/plancat.h"

#define MAX_AO_TUPLE_ID_BUFFER 25
static char AOTupleIdBuffer[MAX_AO_TUPLE_ID_BUFFER];

char *
AOTupleIdToString(AOTupleId *aoTupleId)
{
	int			segmentFileNum = AOTupleIdGet_segmentFileNum(aoTupleId);
	int64		rowNum = AOTupleIdGet_rowNum(aoTupleId);
	int			snprintfResult;

	snprintfResult =
		snprintf(AOTupleIdBuffer, MAX_AO_TUPLE_ID_BUFFER, "(%d," INT64_FORMAT ")",
				 segmentFileNum, rowNum);

	Assert(snprintfResult >= 0);
	Assert(snprintfResult < MAX_AO_TUPLE_ID_BUFFER);

	return AOTupleIdBuffer;
}

/*
 * ao_compute_sample_tuples_per_block
 *
 * Compute the number of tuples per "logical block" for TABLESAMPLE on AO tables.
 *
 * For tables with small tuples (e.g., integers), this returns the maximum
 * value (AO_MAX_TUPLES_PER_HEAP_BLOCK = 32768). For tables with large tuples
 * (e.g., vector types where each tuple is several KB), this returns a
 * proportionally smaller value, making sampling more efficient.
 */
int32
ao_compute_sample_tuples_per_block(Relation rel)
{
	int32	avg_tuple_width;
	int32	tuples_per_block;

	/* Get average tuple width from relation statistics */
	avg_tuple_width = get_rel_data_width(rel, NULL);

	if (avg_tuple_width <= 0)
	{
		/* No statistics available, fall back to default */
		return AO_MAX_TUPLES_PER_HEAP_BLOCK;
	}

	/*
	 * Compute tuples per block based on BLCKSZ.
	 * Use a multiplier of 2 to account for compression and provide margin.
	 * The idea is to make a "logical block" roughly correspond to
	 * a physical storage block's worth of data.
	 */
	tuples_per_block = (BLCKSZ * 2) / avg_tuple_width;

	/* Ensure reasonable bounds */
	tuples_per_block = Max(tuples_per_block, 32);
	tuples_per_block = Min(tuples_per_block, AO_MAX_TUPLES_PER_HEAP_BLOCK);

	return tuples_per_block;
}
