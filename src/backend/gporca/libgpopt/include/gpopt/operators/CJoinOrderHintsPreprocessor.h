//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2024 VMware, Inc. or its affiliates.
//
//	@filename:
//		CJoinOrderHintsPreprocessor.h
//
//	@doc:
//		Preprocessing routines of join order hints
//---------------------------------------------------------------------------

#ifndef GPOPT_CJoinOrderHintsPreprocessor_H
#define GPOPT_CJoinOrderHintsPreprocessor_H

#include "gpos/base.h"
#include "gpos/memory/set.h"

#include "gpopt/base/CColRefSet.h"
#include "gpopt/hints/CHintUtils.h"
#include "gpopt/operators/CExpression.h"

namespace gpopt
{
class CJoinOrderHintsPreprocessor
{
private:
	// Return list of all the children of naryJoinPexpr that are not referenced in
	// binaryJoinExpr, plus binaryJoinExpr itself. *sourceChildIndexes receives,
	// for every returned child, the index of the naryJoinPexpr child it came
	// from, or gpos::ulong_max for binaryJoinExpr.
	static CExpressionArray *ConstructNAryJoinChildren(
		CMemoryPool *mp, CExpression *naryJoinPexpr,
		CExpression *binaryJoinExpr, gpos::set<ULONG> &usedChildrenIndexes,
		ULongPtrArray **sourceChildIndexes);

	// Recursively constructs CLogicalInnerJoin expressions using the children
	// of a CLogicalNAryJoin.
	//
	// usedPredicateIndexes collects the indexes into the CScalarNAryJoinPredList
	// of the LOJ ON predicates the hint consumed; usedInnerConjuncts the
	// conjuncts of its inner-join predicate (index
	// GPOPT_ZERO_INNER_JOIN_PRED_INDEX) the hint consumed, by expression.
	static CExpression *RecursiveApplyJoinOrderHintsOnNAryJoin(
		CMemoryPool *mp, CExpression *pexpr,
		const CJoinHint::JoinNode *joinnode,
		gpos::set<ULONG> &usedChildrenIndexes,
		gpos::set<ULONG> &usedPredicateIndexes,
		gpos::set<CExpression *> &usedInnerConjuncts,
		ULONG *joinChildPredIndex);

public:
	CJoinOrderHintsPreprocessor(const CJoinOrderHintsPreprocessor &) = delete;

	// main driver
	//
	// Search for and apply join order hints on an expression. The result of
	// this converts two or more children of a CLogicalNAryJoin into one or
	// more CLogicalInnerJoin(s).
	static CExpression *PexprPreprocess(CMemoryPool *mp, CExpression *pexpr,
										const CJoinHint::JoinNode *joinnode);
};

}  // namespace gpopt


#endif
