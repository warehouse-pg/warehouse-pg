//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2013 VMware, Inc. or its affiliates.
//
//	@filename:
//		CXformLeftSemiJoin2InnerJoinUnderGb.cpp
//
//	@doc:
//		Implementation of transform
//---------------------------------------------------------------------------

#include "gpopt/xforms/CXformLeftSemiJoin2InnerJoinUnderGb.h"

#include "gpos/base.h"
#include "gpos/memory/CAutoMemoryPool.h"

#include "gpopt/base/CColRefSetIter.h"
#include "gpopt/base/CColRefTable.h"
#include "gpopt/base/CKeyCollection.h"
#include "gpopt/operators/CLogicalGbAggDeduplicate.h"
#include "gpopt/operators/CLogicalInnerJoin.h"
#include "gpopt/operators/CLogicalLeftSemiJoin.h"
#include "gpopt/operators/CPatternLeaf.h"
#include "gpopt/operators/CPredicateUtils.h"
#include "gpopt/operators/CScalarProjectList.h"

using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CXformLeftSemiJoin2InnerJoinUnderGb::CXformLeftSemiJoin2InnerJoinUnderGb
//
//	@doc:
//		ctor
//
//---------------------------------------------------------------------------
CXformLeftSemiJoin2InnerJoinUnderGb::CXformLeftSemiJoin2InnerJoinUnderGb(
	CMemoryPool *mp)
	:  // pattern
	  CXformExploration(GPOS_NEW(mp) CExpression(
		  mp, GPOS_NEW(mp) CLogicalLeftSemiJoin(mp),
		  GPOS_NEW(mp)
			  CExpression(mp, GPOS_NEW(mp) CPatternLeaf(mp)),  // left child
		  GPOS_NEW(mp)
			  CExpression(mp, GPOS_NEW(mp) CPatternLeaf(mp)),  // right child
		  GPOS_NEW(mp)
			  CExpression(mp, GPOS_NEW(mp) CPatternLeaf(mp))  // predicate
		  ))
{
}

//---------------------------------------------------------------------------
//	@function:
//		CXformLeftSemiJoin2InnerJoinUnderGb::Exfp
//
//	@doc:
//		Compute xform promise for a given expression handle;
//
//---------------------------------------------------------------------------
CXform::EXformPromise
CXformLeftSemiJoin2InnerJoinUnderGb::Exfp(CExpressionHandle &exprhdl) const
{
	CColRefSet *pcrsInnerOutput = exprhdl.DeriveOutputColumns(1);
	CExpression *pexprScalar = exprhdl.PexprScalarExactChild(2);
	CAutoMemoryPool amp;
	if (exprhdl.HasOuterRefs() || nullptr == exprhdl.DeriveKeyCollection(0) ||
		nullptr == pexprScalar ||
		CPredicateUtils::FSimpleEqualityUsingCols(amp.Pmp(), pexprScalar,
												  pcrsInnerOutput))
	{
		return ExfpNone;
	}

	return ExfpHigh;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformLeftSemiJoin2InnerJoinUnderGb::Transform
//
//	@doc:
//		actual transformation
//
//---------------------------------------------------------------------------
void
CXformLeftSemiJoin2InnerJoinUnderGb::Transform(CXformContext *pxfctxt,
											   CXformResult *pxfres,
											   CExpression *pexpr) const
{
	GPOS_ASSERT(nullptr != pxfctxt);
	GPOS_ASSERT(FPromising(pxfctxt->Pmp(), this, pexpr));
	GPOS_ASSERT(FCheckPattern(pexpr));

	CMemoryPool *mp = pxfctxt->Pmp();

	// extract components
	CExpression *pexprOuter = (*pexpr)[0];
	CExpression *pexprInner = (*pexpr)[1];
	CExpression *pexprScalar = (*pexpr)[2];

	pexprOuter->AddRef();
	pexprInner->AddRef();
	pexprScalar->AddRef();

	CColRefArray *pdrgpcrKeys = nullptr;
	CColRefArray *pdrgpcrGrouping =
		CUtils::PdrgpcrGroupingKey(mp, pexprOuter, &pdrgpcrKeys);
	GPOS_ASSERT(nullptr != pdrgpcrKeys);

	// Ensure that all grouping columns that are part of a key are marked as used.
	// This prevents key columns from being pruned later (MakeDXLTableDescr),
	// which could cause crashes in semijoin/EXISTS query plans
	// when ORCA generates grouping keys.
	MarkGroupingKeyColsAsUsed(mp, pexprOuter, pdrgpcrGrouping);

	CExpression *pexprInnerJoin = CUtils::PexprLogicalJoin<CLogicalInnerJoin>(
		mp, pexprOuter, pexprInner, pexprScalar);

	CExpression *pexprGb = GPOS_NEW(mp) CExpression(
		mp,
		GPOS_NEW(mp) CLogicalGbAggDeduplicate(
			mp, pdrgpcrGrouping, COperator::EgbaggtypeGlobal /*egbaggtype*/,
			pdrgpcrKeys),
		pexprInnerJoin,
		GPOS_NEW(mp) CExpression(mp, GPOS_NEW(mp) CScalarProjectList(mp)));

	pxfres->Add(pexprGb);
}

//---------------------------------------------------------------------------
//	@function:
//		CXformLeftSemiJoin2InnerJoinUnderGb::MarkGroupingKeyColsAsUsed(
//
//	@doc:
//		Helper function to mark grouping columns that are part of a key
//		as used to prevent pruning.
//
//---------------------------------------------------------------------------
void
CXformLeftSemiJoin2InnerJoinUnderGb::MarkGroupingKeyColsAsUsed(
	CMemoryPool *mp, CExpression *pexprOuter,
	const CColRefArray *pdrgpcrGrouping)
{
	GPOS_ASSERT(nullptr != mp);
	GPOS_ASSERT(nullptr != pexprOuter);
	GPOS_ASSERT(nullptr != pdrgpcrGrouping);

	// Derive the key collection for the outer relation
	CKeyCollection *pkcOuter = pexprOuter->DeriveKeyCollection();

	// Iterate over all grouping columns
	for (ULONG i = 0; i < pdrgpcrGrouping->Size(); i++)
	{
		CColRef *pcr = (*pdrgpcrGrouping)[i];

		// Skip columns that are already marked as used
		if (pcr->GetUsage() == CColRef::EUsed)
		{
			continue;
		}

		// If not set EUsed, check if this column is part of any key in the outer relation
		BOOL found_in_key = false;
		for (ULONG k = 0; k < pkcOuter->Keys() && !found_in_key; k++)
		{
			CColRefSet *pcrsKey = pkcOuter->PcrsKey(mp, k);
			found_in_key = pcrsKey->FMember(pcr);
			pcrsKey->Release();
		}

		// If this column is part of a key, mark it as used to prevent pruning
		if (found_in_key)
		{
			pcr->MarkAsUsed();
		}
	}
}

// EOF
