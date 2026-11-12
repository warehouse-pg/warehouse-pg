//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CXformSelect2WindowTopN.cpp
//
//	@doc:
//		Implementation of the Select over SequenceProject to WindowTopN xform
//---------------------------------------------------------------------------

#include "gpopt/xforms/CXformSelect2WindowTopN.h"

#include "gpos/base.h"

#include "gpopt/base/CColRefSetIter.h"
#include "gpopt/base/COptCtxt.h"
#include "gpopt/base/CWindowOids.h"
#include "gpopt/optimizer/COptimizerConfig.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/base/CWindowFrame.h"
#include "gpopt/operators/CExpressionHandle.h"
#include "gpopt/operators/CLogicalSelect.h"
#include "gpopt/operators/CLogicalSequenceProject.h"
#include "gpopt/operators/CLogicalWindowTopN.h"
#include "gpopt/operators/CPatternLeaf.h"
#include "gpopt/operators/CPatternTree.h"
#include "gpopt/operators/CPredicateUtils.h"
#include "gpopt/operators/CScalarCmp.h"
#include "gpopt/operators/CScalarConst.h"
#include "gpopt/operators/CScalarIdent.h"
#include "gpopt/operators/CScalarProjectElement.h"
#include "gpopt/operators/CScalarWindowFunc.h"
#include "naucrates/base/IDatumInt2.h"
#include "naucrates/base/IDatumInt4.h"
#include "naucrates/base/IDatumInt8.h"
#include "naucrates/md/CMDIdGPDB.h"

using namespace gpopt;
using namespace gpmd;

//---------------------------------------------------------------------------
//	@function:
//		CXformSelect2WindowTopN::CXformSelect2WindowTopN
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CXformSelect2WindowTopN::CXformSelect2WindowTopN(CMemoryPool *mp)
	: CXformExploration(
		  // pattern
		  GPOS_NEW(mp) CExpression(
			  mp, GPOS_NEW(mp) CLogicalSelect(mp),
			  GPOS_NEW(mp) CExpression(
				  mp, GPOS_NEW(mp) CLogicalSequenceProject(mp),
				  GPOS_NEW(mp) CExpression(
					  mp, GPOS_NEW(mp) CPatternLeaf(mp)),  // relational child
				  GPOS_NEW(mp) CExpression(
					  mp, GPOS_NEW(mp) CPatternTree(mp))  // project list
				  ),
			  GPOS_NEW(mp) CExpression(
				  mp, GPOS_NEW(mp) CPatternTree(mp))  // predicate
			  ))
{
}

//---------------------------------------------------------------------------
//	@function:
//		CXformSelect2WindowTopN::Exfp
//
//	@doc:
//		Compute xform promise for a given expression handle
//
//---------------------------------------------------------------------------
CXform::EXformPromise
CXformSelect2WindowTopN::Exfp(CExpressionHandle &exprhdl) const
{
	if (exprhdl.DeriveHasSubquery(1) ||
		0 < exprhdl.DeriveOuterReferences()->Size())
	{
		return CXform::ExfpNone;
	}

	return CXform::ExfpHigh;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformSelect2WindowTopN::EwtnKind
//
//	@doc:
//		Kind of a project element computing row_number(), rank() or
//		dense_rank(); EwtnSentinel for anything else
//
//---------------------------------------------------------------------------
EWindowTopNKind
CXformSelect2WindowTopN::EwtnKind(CExpression *pexprPrEl)
{
	CExpression *pexpr = (*pexprPrEl)[0];

	if (COperator::EopScalarWindowFunc != pexpr->Pop()->Eopid())
	{
		return EwtnSentinel;
	}

	CScalarWindowFunc *popWinFunc = CScalarWindowFunc::PopConvert(pexpr->Pop());
	if (0 != pexpr->Arity() || popWinFunc->IsDistinct())
	{
		return EwtnSentinel;
	}

	OID oid = CMDIdGPDB::CastMdid(popWinFunc->FuncMdId())->Oid();
	const CWindowOids *pwindowoids =
		COptCtxt::PoctxtFromTLS()->GetOptimizerConfig()->GetWindowOids();

	if (oid == pwindowoids->OidRowNumber())
	{
		return EwtnRowNumber;
	}
	if (oid == pwindowoids->OidRank())
	{
		return EwtnRank;
	}
	if (oid == pwindowoids->OidDenseRank())
	{
		return EwtnDenseRank;
	}

	return EwtnSentinel;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformSelect2WindowTopN::FPrefilterSafe
//
//	@doc:
//		The rows kept are, per partition and in window order, a prefix
//		(ending after a complete peer group for rank() and dense_rank(),
//		possibly inside one for row_number()).  Every window function of
//		the project list must therefore only look at the current row and
//		the rows before it: the ranking functions themselves, or aggregates
//		whose frame ends at the current row (in ROWS mode when the prefix
//		may cut a peer group).
//
//---------------------------------------------------------------------------
BOOL
CXformSelect2WindowTopN::FPrefilterSafe(CExpression *pexprPrL,
										CWindowFrame *pwf,
										BOOL fRowNumberBound)
{
	const ULONG arity = pexprPrL->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CExpression *pexprPrEl = (*pexprPrL)[ul];
		CExpression *pexpr = (*pexprPrEl)[0];

		if (COperator::EopScalarWindowFunc != pexpr->Pop()->Eopid())
		{
			continue;
		}
		if (EwtnSentinel != EwtnKind(pexprPrEl))
		{
			continue;
		}

		CScalarWindowFunc *popWinFunc =
			CScalarWindowFunc::PopConvert(pexpr->Pop());
		if (!popWinFunc->FAgg())
		{
			return false;
		}

		// an empty frame stands for the default RANGE ... CURRENT ROW
		CWindowFrame::EFrameBoundary efbTrailing = CWindowFrame::EfbCurrentRow;
		CWindowFrame::EFrameSpec efs = CWindowFrame::EfsRange;
		if (nullptr != pwf && !CWindowFrame::IsEmpty(pwf))
		{
			efbTrailing = pwf->EfbTrailing();
			efs = pwf->Efs();
		}

		if (CWindowFrame::EfbCurrentRow != efbTrailing)
		{
			return false;
		}
		if (fRowNumberBound && CWindowFrame::EfsRows != efs)
		{
			return false;
		}
	}

	return true;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformSelect2WindowTopN::FFindBound
//
//	@doc:
//		Look for a conjunct "col <op> const" where col is a ranking window
//		function of the project list and <op> bounds it from above; with
//		several, take the smallest bound
//
//---------------------------------------------------------------------------
BOOL
CXformSelect2WindowTopN::FFindBound(CMemoryPool *mp, CExpression *pexprPred,
									CExpression *pexprPrL,
									EWindowTopNKind *pkind, LINT *plimit)
{
	BOOL fFound = false;
	CExpressionArray *pdrgpexpr = CPredicateUtils::PdrgpexprConjuncts(mp, pexprPred);

	for (ULONG ul = 0; ul < pdrgpexpr->Size(); ul++)
	{
		CExpression *pexprConj = (*pdrgpexpr)[ul];

		if (COperator::EopScalarCmp != pexprConj->Pop()->Eopid() ||
			2 != pexprConj->Arity())
		{
			continue;
		}

		CExpression *pexprLeft = (*pexprConj)[0];
		CExpression *pexprRight = (*pexprConj)[1];
		CExpression *pexprIdent;
		CExpression *pexprConst;
		BOOL fIdentLeft;

		if (CUtils::FScalarIdent(pexprLeft) && CUtils::FScalarConst(pexprRight))
		{
			pexprIdent = pexprLeft;
			pexprConst = pexprRight;
			fIdentLeft = true;
		}
		else if (CUtils::FScalarConst(pexprLeft) &&
				 CUtils::FScalarIdent(pexprRight))
		{
			pexprIdent = pexprRight;
			pexprConst = pexprLeft;
			fIdentLeft = false;
		}
		else
		{
			continue;
		}

		// the comparison as "ident <op> const"
		IMDType::ECmpType ecmpt =
			CScalarCmp::PopConvert(pexprConj->Pop())->ParseCmpType();
		if (!fIdentLeft)
		{
			switch (ecmpt)
			{
				case IMDType::EcmptL:
					ecmpt = IMDType::EcmptG;
					break;
				case IMDType::EcmptLEq:
					ecmpt = IMDType::EcmptGEq;
					break;
				case IMDType::EcmptG:
					ecmpt = IMDType::EcmptL;
					break;
				case IMDType::EcmptGEq:
					ecmpt = IMDType::EcmptLEq;
					break;
				default:
					break;
			}
		}
		if (IMDType::EcmptL != ecmpt && IMDType::EcmptLEq != ecmpt &&
			IMDType::EcmptEq != ecmpt)
		{
			continue;
		}

		// the constant must be a non-null integer
		IDatum *datum = CScalarConst::PopConvert(pexprConst->Pop())->GetDatum();
		if (datum->IsNull())
		{
			continue;
		}
		LINT value;
		switch (datum->GetDatumType())
		{
			case IMDType::EtiInt2:
				value = dynamic_cast<IDatumInt2 *>(datum)->Value();
				break;
			case IMDType::EtiInt4:
				value = dynamic_cast<IDatumInt4 *>(datum)->Value();
				break;
			case IMDType::EtiInt8:
				value = dynamic_cast<IDatumInt8 *>(datum)->Value();
				break;
			default:
				continue;
		}
		if (IMDType::EcmptL == ecmpt)
		{
			value--;
		}

		// the column must be a ranking function of this window
		const CColRef *colref =
			CScalarIdent::PopConvert(pexprIdent->Pop())->Pcr();
		EWindowTopNKind kind = EwtnSentinel;
		for (ULONG ulPrEl = 0; ulPrEl < pexprPrL->Arity(); ulPrEl++)
		{
			CExpression *pexprPrEl = (*pexprPrL)[ulPrEl];

			if (CScalarProjectElement::PopConvert(pexprPrEl->Pop())->Pcr() ==
				colref)
			{
				kind = EwtnKind(pexprPrEl);
				break;
			}
		}
		if (EwtnSentinel == kind)
		{
			continue;
		}

		if (!fFound || value < *plimit)
		{
			fFound = true;
			*pkind = kind;
			*plimit = value;
		}
	}

	pdrgpexpr->Release();
	return fFound;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformSelect2WindowTopN::Transform
//
//	@doc:
//		Actual transformation
//
//---------------------------------------------------------------------------
void
CXformSelect2WindowTopN::Transform(CXformContext *pxfctxt,
								   CXformResult *pxfres,
								   CExpression *pexpr) const
{
	GPOS_ASSERT(nullptr != pxfctxt);
	GPOS_ASSERT(FPromising(pxfctxt->Pmp(), this, pexpr));
	GPOS_ASSERT(FCheckPattern(pexpr));

	CMemoryPool *mp = pxfctxt->Pmp();

	CExpression *pexprSeqPrj = (*pexpr)[0];
	CExpression *pexprPred = (*pexpr)[1];
	CExpression *pexprChild = (*pexprSeqPrj)[0];
	CExpression *pexprPrL = (*pexprSeqPrj)[1];
	CLogicalSequenceProject *popSeqPrj =
		CLogicalSequenceProject::PopConvert(pexprSeqPrj->Pop());

	// one window specification per SequenceProject
	if (1 != popSeqPrj->Pdrgpos()->Size() || 1 != popSeqPrj->Pdrgpwf()->Size())
	{
		return;
	}
	COrderSpec *pos = (*popSeqPrj->Pdrgpos())[0];
	CWindowFrame *pwf = (*popSeqPrj->Pdrgpwf())[0];

	EWindowTopNKind kind = EwtnSentinel;
	LINT limit = 0;
	if (!FFindBound(mp, pexprPred, pexprPrL, &kind, &limit))
	{
		return;
	}

	// nothing to gain from a bound no row can meet; leave it to the filter
	if (limit <= 0)
	{
		return;
	}

	// rank() and dense_rank() are 1 for every row without ORDER BY
	if (EwtnRowNumber != kind && pos->IsEmpty())
	{
		return;
	}

	if (!FPrefilterSafe(pexprPrL, pwf, EwtnRowNumber == kind))
	{
		return;
	}

	// the node hashes its input on the partition columns
	CDistributionSpec *pds = popSeqPrj->Pds();
	CColRefSet *pcrsPart = pds->PcrsUsed(mp);
	BOOL fHashable = true;
	CColRefSetIter crsi(*pcrsPart);
	while (fHashable && crsi.Advance())
	{
		fHashable = crsi.Pcr()->RetrieveType()->IsHashable();
	}
	pcrsPart->Release();
	if (!fHashable)
	{
		return;
	}

	// WindowTopN(child)
	pds->AddRef();
	pos->AddRef();
	pexprChild->AddRef();
	CExpression *pexprTopN = GPOS_NEW(mp) CExpression(
		mp, GPOS_NEW(mp) CLogicalWindowTopN(mp, pds, pos, kind, limit),
		pexprChild);

	// SequenceProject(WindowTopN(child))
	pds->AddRef();
	popSeqPrj->Pdrgpos()->AddRef();
	popSeqPrj->Pdrgpwf()->AddRef();
	pexprPrL->AddRef();
	CExpression *pexprNewSeqPrj = GPOS_NEW(mp) CExpression(
		mp,
		GPOS_NEW(mp) CLogicalSequenceProject(mp, pds, popSeqPrj->Pdrgpos(),
											 popSeqPrj->Pdrgpwf()),
		pexprTopN, pexprPrL);

	// Select(SequenceProject(WindowTopN(child)))
	pexprPred->AddRef();
	CExpression *pexprResult = GPOS_NEW(mp) CExpression(
		mp, GPOS_NEW(mp) CLogicalSelect(mp), pexprNewSeqPrj, pexprPred);

	pxfres->Add(pexprResult);
}

// EOF
