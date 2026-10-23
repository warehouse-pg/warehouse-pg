//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CXformImplementWindowTopN.cpp
//
//	@doc:
//		Implementation of the logical to physical WindowTopN xform
//---------------------------------------------------------------------------

#include "gpopt/xforms/CXformImplementWindowTopN.h"

#include "gpos/base.h"

#include "gpopt/operators/CLogicalWindowTopN.h"
#include "gpopt/operators/CPatternLeaf.h"
#include "gpopt/operators/CPhysicalWindowTopN.h"

using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CXformImplementWindowTopN::CXformImplementWindowTopN
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CXformImplementWindowTopN::CXformImplementWindowTopN(CMemoryPool *mp)
	:  // pattern
	  CXformImplementation(GPOS_NEW(mp) CExpression(
		  mp, GPOS_NEW(mp) CLogicalWindowTopN(mp),
		  GPOS_NEW(mp) CExpression(
			  mp, GPOS_NEW(mp) CPatternLeaf(mp))	 // relational child
		  ))
{
}

//---------------------------------------------------------------------------
//	@function:
//		CXformImplementWindowTopN::Transform
//
//	@doc:
//		Actual transformation
//
//---------------------------------------------------------------------------
void
CXformImplementWindowTopN::Transform(CXformContext *pxfctxt,
									 CXformResult *pxfres,
									 CExpression *pexpr) const
{
	GPOS_ASSERT(nullptr != pxfctxt);
	GPOS_ASSERT(FPromising(pxfctxt->Pmp(), this, pexpr));
	GPOS_ASSERT(FCheckPattern(pexpr));

	CMemoryPool *mp = pxfctxt->Pmp();

	CLogicalWindowTopN *popTopN = CLogicalWindowTopN::PopConvert(pexpr->Pop());
	CExpression *pexprRelational = (*pexpr)[0];

	pexprRelational->AddRef();
	popTopN->Pds()->AddRef();
	popTopN->Pos()->AddRef();

	CExpression *pexprTopN = GPOS_NEW(mp) CExpression(
		mp,
		GPOS_NEW(mp) CPhysicalWindowTopN(mp, popTopN->Pds(), popTopN->Pos(),
										 popTopN->Kind(), popTopN->Limit()),
		pexprRelational);

	pxfres->Add(pexprTopN);
}

// EOF
