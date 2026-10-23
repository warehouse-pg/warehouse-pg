//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CXformImplementWindowTopN.h
//
//	@doc:
//		Transform logical WindowTopN into physical WindowTopN
//---------------------------------------------------------------------------
#ifndef GPOPT_CXformImplementWindowTopN_H
#define GPOPT_CXformImplementWindowTopN_H

#include "gpos/base.h"

#include "gpopt/xforms/CXformImplementation.h"

namespace gpopt
{
using namespace gpos;

//---------------------------------------------------------------------------
//	@class:
//		CXformImplementWindowTopN
//
//	@doc:
//		Transform logical WindowTopN into physical WindowTopN
//
//---------------------------------------------------------------------------
class CXformImplementWindowTopN : public CXformImplementation
{
public:
	CXformImplementWindowTopN(const CXformImplementWindowTopN &) = delete;

	// ctor
	explicit CXformImplementWindowTopN(CMemoryPool *mp);

	// dtor
	~CXformImplementWindowTopN() override = default;

	// ident accessors
	EXformId
	Exfid() const override
	{
		return ExfImplementWindowTopN;
	}

	// return a string for xform name
	const CHAR *
	SzId() const override
	{
		return "CXformImplementWindowTopN";
	}

	// compute xform promise for a given expression handle
	EXformPromise
	Exfp(CExpressionHandle &  // exprhdl
	) const override
	{
		return CXform::ExfpHigh;
	}

	// actual transform
	void Transform(CXformContext *pxfctxt, CXformResult *pxfres,
				   CExpression *pexpr) const override;

};	// class CXformImplementWindowTopN
}  // namespace gpopt

#endif	// !GPOPT_CXformImplementWindowTopN_H

// EOF
