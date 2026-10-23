//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CXformSelect2WindowTopN.h
//
//	@doc:
//		Transform Select(SequenceProject(X)) with a predicate bounding a
//		rank-like window function into
//		Select(SequenceProject(WindowTopN(X)))
//---------------------------------------------------------------------------
#ifndef GPOPT_CXformSelect2WindowTopN_H
#define GPOPT_CXformSelect2WindowTopN_H

#include "gpos/base.h"

#include "gpopt/operators/CLogicalWindowTopN.h"
#include "gpopt/xforms/CXformExploration.h"

namespace gpopt
{
using namespace gpos;

//---------------------------------------------------------------------------
//	@class:
//		CXformSelect2WindowTopN
//
//	@doc:
//		Insert a per-partition top-N below a filtered window
//
//---------------------------------------------------------------------------
class CXformSelect2WindowTopN : public CXformExploration
{
private:
	// does the window function allow rows beyond the bound to be removed
	// before the window is evaluated?
	static BOOL FPrefilterSafe(CExpression *pexprPrL, CWindowFrame *pwf,
							   BOOL fRowNumberBound);

	// find the bound in the predicate; returns false if there is none
	static BOOL FFindBound(CMemoryPool *mp, CExpression *pexprPred,
						   CExpression *pexprPrL, EWindowTopNKind *pkind,
						   LINT *plimit);

	// window function kind of a project element, EwtnSentinel if none
	static EWindowTopNKind EwtnKind(CExpression *pexprPrEl);

public:
	CXformSelect2WindowTopN(const CXformSelect2WindowTopN &) = delete;

	// ctor
	explicit CXformSelect2WindowTopN(CMemoryPool *mp);

	// dtor
	~CXformSelect2WindowTopN() override = default;

	// ident accessors
	EXformId
	Exfid() const override
	{
		return ExfSelect2WindowTopN;
	}

	// return a string for xform name
	const CHAR *
	SzId() const override
	{
		return "CXformSelect2WindowTopN";
	}

	// the result still matches the pattern; do not apply the xform to it
	BOOL
	FCompatible(CXform::EXformId exfid) override
	{
		return (CXform::ExfSelect2WindowTopN != exfid);
	}

	// compute xform promise for a given expression handle
	EXformPromise Exfp(CExpressionHandle &exprhdl) const override;

	// actual transform
	void Transform(CXformContext *pxfctxt, CXformResult *pxfres,
				   CExpression *pexpr) const override;

};	// class CXformSelect2WindowTopN
}  // namespace gpopt

#endif	// !GPOPT_CXformSelect2WindowTopN_H

// EOF
