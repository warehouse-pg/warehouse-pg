//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Corp.
//
//	@filename:
//		CConstraintTest.cpp
//
//	@doc:
//		Test for constraint
//---------------------------------------------------------------------------

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#include "unittest/gpopt/base/CConstraintTest.h"

#include <stdint.h>

#include "gpos/task/CAutoTraceFlag.h"

#include "gpopt/base/CDefaultComparator.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/engine/CCTEConfig.h"
#include "gpopt/engine/CEnumeratorConfig.h"
#include "gpopt/engine/CHint.h"
#include "gpopt/engine/CStatisticsConfig.h"
#include "gpopt/eval/CConstExprEvaluatorDefault.h"
#include "gpopt/exception.h"
#include "gpopt/operators/CPredicateUtils.h"
#include "gpopt/optimizer/COptimizerConfig.h"
#include "naucrates/base/CDatumInt8GPDB.h"
#include "naucrates/md/CMDIdGPDB.h"
#include "naucrates/md/IMDScalarOp.h"

#include "unittest/base.h"
#include "unittest/gpopt/CConstExprEvaluatorForDates.h"

// number of microseconds in one day
const LINT CConstraintTest::lMicrosecondsPerDay =
	24 * 60 * 60 * INT64_C(1000000);

// date for '01-01-2012'
const LINT CConstraintTest::lInternalRepresentationFor2012_01_01 =
	LINT(4383) * CConstraintTest::lMicrosecondsPerDay;

// date for '01-21-2012'
const LINT CConstraintTest::lInternalRepresentationFor2012_01_21 =
	LINT(5003) * CConstraintTest::lMicrosecondsPerDay;

// date for '01-02-2012'
const LINT CConstraintTest::lInternalRepresentationFor2012_01_02 =
	LINT(4384) * CConstraintTest::lMicrosecondsPerDay;

// date for '01-22-2012'
const LINT CConstraintTest::lInternalRepresentationFor2012_01_22 =
	LINT(5004) * CConstraintTest::lMicrosecondsPerDay;

// byte representation for '01-01-2012'
const WCHAR *CConstraintTest::wszInternalRepresentationFor2012_01_01 =
	GPOS_WSZ_LIT("HxEAAA==");

// byte representation for '01-21-2012'
const WCHAR *CConstraintTest::wszInternalRepresentationFor2012_01_21 =
	GPOS_WSZ_LIT("MxEAAA==");

// byte representation for '01-02-2012'
const WCHAR *CConstraintTest::wszInternalRepresentationFor2012_01_02 =
	GPOS_WSZ_LIT("IBEAAA==");

// byte representation for '01-22-2012'
const WCHAR *CConstraintTest::wszInternalRepresentationFor2012_01_22 =
	GPOS_WSZ_LIT("MhEAAA==");

static GPOS_RESULT EresUnittest_CConstraintIntervalFromArrayExprIncludesNull();
static GPOS_RESULT EresUnittest_CConstraintIntervalArrayCache();
static GPOS_RESULT EresUnittest_CConstraintIntervalArrayThreshold();

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest
//
//	@doc:
//		Unittest for ranges
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest()
{
	CUnittest rgut[] = {
		GPOS_UNITTEST_FUNC(
			EresUnittest_CConstraintIntervalFromArrayExprIncludesNull),
		GPOS_UNITTEST_FUNC(EresUnittest_CConstraintIntervalArrayCache),
		GPOS_UNITTEST_FUNC(EresUnittest_CConstraintIntervalArrayThreshold),
		GPOS_UNITTEST_FUNC(CConstraintTest::EresUnittest_CInterval),
		GPOS_UNITTEST_FUNC(
			CConstraintTest::EresUnittest_CIntervalFromScalarExpr),
		GPOS_UNITTEST_FUNC(CConstraintTest::EresUnittest_CConjunction),
		GPOS_UNITTEST_FUNC(CConstraintTest::EresUnittest_CDisjunction),
		GPOS_UNITTEST_FUNC(CConstraintTest::EresUnittest_CNegation),
		GPOS_UNITTEST_FUNC(
			CConstraintTest::EresUnittest_CConstraintFromScalarExpr),
		GPOS_UNITTEST_FUNC(
			CConstraintTest::EresUnittest_CConstraintIntervalConvertsTo),
		GPOS_UNITTEST_FUNC(
			CConstraintTest::EresUnittest_CConstraintIntervalPexpr),
		GPOS_UNITTEST_FUNC(
			CConstraintTest::EresUnittest_CConstraintIntervalFromArrayExpr),
#ifdef GPOS_DEBUG
		GPOS_UNITTEST_FUNC_THROW(CConstraintTest::EresUnittest_NegativeTests,
								 gpopt::ExmaGPOPT, gpopt::ExmiUnsupportedOp),
#endif	// GPOS_DEBUG
		GPOS_UNITTEST_FUNC(CConstraintTest::EresUnittest_ConstraintsOnDates),
	};

	return CUnittest::EresExecute(rgut, GPOS_ARRAY_SIZE(rgut));
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CInterval
//
//	@doc:
//		Interval tests
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CInterval()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	IMDTypeInt8 *pmdtypeint8 =
		(IMDTypeInt8 *) mda.PtMDType<IMDTypeInt8>(CTestUtils::m_sysidDefault);
	IMDId *mdid = pmdtypeint8->MDId();

	CExpression *pexprGet = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs = pexprGet->DeriveOutputColumns();
	CColRef *colref = pcrs->PcrAny();

	// first interval
	CConstraintInterval *pciFirst = PciFirstInterval(mp, mdid, colref);
	PrintConstraint(mp, pciFirst);

	// second interval
	CConstraintInterval *pciSecond = PciSecondInterval(mp, mdid, colref);
	PrintConstraint(mp, pciSecond);

	// intersection
	CConstraintInterval *pciIntersect = pciFirst->PciIntersect(mp, pciSecond);
	PrintConstraint(mp, pciIntersect);

	// union
	CConstraintInterval *pciUnion = pciFirst->PciUnion(mp, pciSecond);
	PrintConstraint(mp, pciUnion);

	// diff 1
	CConstraintInterval *pciDiff1 = pciFirst->PciDifference(mp, pciSecond);
	PrintConstraint(mp, pciDiff1);

	// diff 2
	CConstraintInterval *pciDiff2 = pciSecond->PciDifference(mp, pciFirst);
	PrintConstraint(mp, pciDiff2);

	// complement
	CConstraintInterval *pciComp = pciFirst->PciComplement(mp);
	PrintConstraint(mp, pciComp);

	// containment
	GPOS_UNITTEST_ASSERT(!pciFirst->Contains(pciSecond));
	GPOS_UNITTEST_ASSERT(pciFirst->Contains(pciDiff1));
	GPOS_UNITTEST_ASSERT(!pciSecond->Contains(pciFirst));
	GPOS_UNITTEST_ASSERT(pciSecond->Contains(pciDiff2));

	// equality
	CConstraintInterval *pciThird = PciFirstInterval(mp, mdid, colref);
	pciThird->AddRef();
	CConstraintInterval *pciFourth = pciThird;
	GPOS_UNITTEST_ASSERT(!pciFirst->Equals(pciSecond));
	GPOS_UNITTEST_ASSERT(!pciFirst->Equals(pciDiff1));
	GPOS_UNITTEST_ASSERT(!pciSecond->Equals(pciDiff2));
	GPOS_UNITTEST_ASSERT(pciFirst->Equals(pciThird));
	GPOS_UNITTEST_ASSERT(pciFourth->Equals(pciThird));

	pciFirst->Release();
	pciSecond->Release();
	pciThird->Release();
	pciFourth->Release();
	pciIntersect->Release();
	pciUnion->Release();
	pciDiff1->Release();
	pciDiff2->Release();
	pciComp->Release();

	pexprGet->Release();

	return GPOS_OK;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CConjunction
//
//	@doc:
//		Conjunction test
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CConjunction()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	IMDTypeInt8 *pmdtypeint8 =
		(IMDTypeInt8 *) mda.PtMDType<IMDTypeInt8>(CTestUtils::m_sysidDefault);
	IMDId *mdid = pmdtypeint8->MDId();

	CExpression *pexprGet1 = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs1 = pexprGet1->DeriveOutputColumns();
	CColRef *pcr1 = pcrs1->PcrAny();

	CExpression *pexprGet2 = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs2 = pexprGet2->DeriveOutputColumns();
	CColRef *pcr2 = pcrs2->PcrAny();

	CConstraintConjunction *pcconj1 = Pcstconjunction(mp, mdid, pcr1);
	PrintConstraint(mp, pcconj1);
	GPOS_UNITTEST_ASSERT(!pcconj1->FContradiction());

	CConstraintConjunction *pcconj2 = Pcstconjunction(mp, mdid, pcr2);
	PrintConstraint(mp, pcconj2);

	CConstraintArray *pdrgpcst = GPOS_NEW(mp) CConstraintArray(mp);
	pcconj1->AddRef();
	pcconj2->AddRef();
	pdrgpcst->Append(pcconj1);
	pdrgpcst->Append(pcconj2);

	CConstraintConjunction *pcconjTop =
		GPOS_NEW(mp) CConstraintConjunction(mp, pdrgpcst);
	PrintConstraint(mp, pcconjTop);

	// containment
	GPOS_UNITTEST_ASSERT(!pcconj1->Contains(pcconj2));
	GPOS_UNITTEST_ASSERT(!pcconj2->Contains(pcconj1));
	GPOS_UNITTEST_ASSERT(pcconj1->Contains(pcconjTop));
	GPOS_UNITTEST_ASSERT(!pcconjTop->Contains(pcconj1));
	GPOS_UNITTEST_ASSERT(pcconj2->Contains(pcconjTop));
	GPOS_UNITTEST_ASSERT(!pcconjTop->Contains(pcconj2));

	// equality
	GPOS_UNITTEST_ASSERT(!pcconj1->Equals(pcconjTop));
	GPOS_UNITTEST_ASSERT(!pcconjTop->Equals(pcconj2));

	pcconjTop->Release();
	pcconj1->Release();
	pcconj2->Release();

	pexprGet1->Release();
	pexprGet2->Release();

	return GPOS_OK;
}


//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::Pcstconjunction
//
//	@doc:
//		Build a conjunction
//
//---------------------------------------------------------------------------
CConstraintConjunction *
CConstraintTest::Pcstconjunction(CMemoryPool *mp, IMDId *mdid, CColRef *colref)
{
	// first interval
	CConstraintInterval *pciFirst = PciFirstInterval(mp, mdid, colref);

	// second interval
	CConstraintInterval *pciSecond = PciSecondInterval(mp, mdid, colref);

	CConstraintArray *pdrgpcst = GPOS_NEW(mp) CConstraintArray(mp);
	pdrgpcst->Append(pciFirst);
	pdrgpcst->Append(pciSecond);

	return GPOS_NEW(mp) CConstraintConjunction(mp, pdrgpcst);
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::Pcstdisjunction
//
//	@doc:
//		Build a disjunction
//
//---------------------------------------------------------------------------
CConstraintDisjunction *
CConstraintTest::Pcstdisjunction(CMemoryPool *mp, IMDId *mdid, CColRef *colref)
{
	// first interval
	CConstraintInterval *pciFirst = PciFirstInterval(mp, mdid, colref);

	// second interval
	CConstraintInterval *pciSecond = PciSecondInterval(mp, mdid, colref);

	CConstraintArray *pdrgpcst = GPOS_NEW(mp) CConstraintArray(mp);
	pdrgpcst->Append(pciFirst);
	pdrgpcst->Append(pciSecond);

	return GPOS_NEW(mp) CConstraintDisjunction(mp, pdrgpcst);
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CDisjunction
//
//	@doc:
//		Conjunction test
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CDisjunction()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	IMDTypeInt8 *pmdtypeint8 =
		(IMDTypeInt8 *) mda.PtMDType<IMDTypeInt8>(CTestUtils::m_sysidDefault);
	IMDId *mdid = pmdtypeint8->MDId();

	CExpression *pexprGet1 = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs1 = pexprGet1->DeriveOutputColumns();
	CColRef *pcr1 = pcrs1->PcrAny();

	CExpression *pexprGet2 = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs2 = pexprGet2->DeriveOutputColumns();
	CColRef *pcr2 = pcrs2->PcrAny();

	CConstraintDisjunction *pcdisj1 = Pcstdisjunction(mp, mdid, pcr1);
	PrintConstraint(mp, pcdisj1);
	GPOS_UNITTEST_ASSERT(!pcdisj1->FContradiction());

	CConstraintDisjunction *pcdisj2 = Pcstdisjunction(mp, mdid, pcr2);
	PrintConstraint(mp, pcdisj2);

	CConstraintArray *pdrgpcst = GPOS_NEW(mp) CConstraintArray(mp);
	pcdisj1->AddRef();
	pcdisj2->AddRef();
	pdrgpcst->Append(pcdisj1);
	pdrgpcst->Append(pcdisj2);

	CConstraintDisjunction *pcdisjTop =
		GPOS_NEW(mp) CConstraintDisjunction(mp, pdrgpcst);
	PrintConstraint(mp, pcdisjTop);

	// containment
	GPOS_UNITTEST_ASSERT(!pcdisj1->Contains(pcdisj2));
	GPOS_UNITTEST_ASSERT(!pcdisj2->Contains(pcdisj1));
	GPOS_UNITTEST_ASSERT(!pcdisj1->Contains(pcdisjTop));
	GPOS_UNITTEST_ASSERT(pcdisjTop->Contains(pcdisj1));
	GPOS_UNITTEST_ASSERT(!pcdisj2->Contains(pcdisjTop));
	GPOS_UNITTEST_ASSERT(pcdisjTop->Contains(pcdisj2));

	// equality
	GPOS_UNITTEST_ASSERT(!pcdisj1->Equals(pcdisjTop));
	GPOS_UNITTEST_ASSERT(!pcdisjTop->Equals(pcdisj2));

	pcdisjTop->Release();
	pcdisj1->Release();
	pcdisj2->Release();

	pexprGet1->Release();
	pexprGet2->Release();

	return GPOS_OK;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CNegation
//
//	@doc:
//		Conjunction test
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CNegation()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	IMDTypeInt8 *pmdtypeint8 =
		(IMDTypeInt8 *) mda.PtMDType<IMDTypeInt8>(CTestUtils::m_sysidDefault);
	IMDId *mdid = pmdtypeint8->MDId();

	CExpression *pexprGet = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs = pexprGet->DeriveOutputColumns();
	CColRef *colref = pcrs->PcrAny();

	CConstraintInterval *pci = PciFirstInterval(mp, mdid, colref);
	PrintConstraint(mp, pci);

	pci->AddRef();
	CConstraintNegation *pcn1 = GPOS_NEW(mp) CConstraintNegation(mp, pci);
	PrintConstraint(mp, pcn1);

	pcn1->AddRef();
	CConstraintNegation *pcn2 = GPOS_NEW(mp) CConstraintNegation(mp, pcn1);
	PrintConstraint(mp, pcn2);

	// containment
	GPOS_UNITTEST_ASSERT(!pcn1->Contains(pci));
	GPOS_UNITTEST_ASSERT(!pci->Contains(pcn1));
	GPOS_UNITTEST_ASSERT(!pcn2->Contains(pcn1));
	GPOS_UNITTEST_ASSERT(!pcn1->Contains(pcn2));
	GPOS_UNITTEST_ASSERT(pci->Contains(pcn2));
	GPOS_UNITTEST_ASSERT(pcn2->Contains(pci));

	// equality
	GPOS_UNITTEST_ASSERT(!pcn1->Equals(pci));
	GPOS_UNITTEST_ASSERT(!pcn1->Equals(pcn2));
	GPOS_UNITTEST_ASSERT(pci->Equals(pcn2));

	pcn2->Release();
	pcn1->Release();
	pci->Release();

	pexprGet->Release();

	return GPOS_OK;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CIntervalFromScalarExpr
//
//	@doc:
//		Interval from scalar tests
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CIntervalFromScalarExpr()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	CExpression *pexprGet = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs = pexprGet->DeriveOutputColumns();
	CColRef *colref = pcrs->PcrAny();

	// from ScalarCmp
	GPOS_RESULT eres1 = EresUnittest_CIntervalFromScalarCmp(mp, &mda, colref);

	// from ScalarBool
	GPOS_RESULT eres2 =
		EresUnittest_CIntervalFromScalarBoolOp(mp, &mda, colref);

	pexprGet->Release();
	if (GPOS_OK == eres1 && GPOS_OK == eres2)
	{
		return GPOS_OK;
	}

	return GPOS_FAILED;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CConstraintFromScalarExpr
//
//	@doc:
//		Constraint from scalar tests
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CConstraintFromScalarExpr()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	CExpression *pexprGet1 = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs1 = pexprGet1->DeriveOutputColumns();
	CColRef *pcr1 = pcrs1->PcrAny();

	CExpression *pexprGet2 = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs2 = pexprGet2->DeriveOutputColumns();
	CColRef *pcr2 = pcrs2->PcrAny();

	CColRefSetArray *pdrgpcrs = nullptr;

	// expression with 1 column
	CExpression *pexpr = PexprScalarCmp(mp, &mda, pcr1, IMDType::EcmptG, 15);
	CConstraint *pcnstr =
		CConstraint::PcnstrFromScalarExpr(mp, pexpr, &pdrgpcrs);
	PrintConstraint(mp, pcnstr);
	PrintEquivClasses(mp, pdrgpcrs);
	pdrgpcrs->Release();
	pdrgpcrs = nullptr;
	pcnstr->Release();
	pexpr->Release();

	// expression with 2 columns
	CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
	pdrgpexpr->Append(PexprScalarCmp(mp, &mda, pcr1, IMDType::EcmptG, 15));
	pdrgpexpr->Append(PexprScalarCmp(mp, &mda, pcr2, IMDType::EcmptL, 20));

	CExpression *pexprAnd =
		CUtils::PexprScalarBoolOp(mp, CScalarBoolOp::EboolopAnd, pdrgpexpr);
	(void) pexprAnd->PdpDerive();

	CConstraint *pcnstrAnd =
		CConstraint::PcnstrFromScalarExpr(mp, pexprAnd, &pdrgpcrs);
	PrintConstraint(mp, pcnstrAnd);
	PrintEquivClasses(mp, pdrgpcrs);
	pdrgpcrs->Release();
	pdrgpcrs = nullptr;

	pcnstrAnd->Release();
	pexprAnd->Release();

	// equality predicate with 2 columns
	CExpression *pexprEq = CUtils::PexprScalarEqCmp(mp, pcr1, pcr2);
	(void) pexprEq->PdpDerive();
	CConstraint *pcnstrEq =
		CConstraint::PcnstrFromScalarExpr(mp, pexprEq, &pdrgpcrs);
	PrintConstraint(mp, pcnstrEq);
	PrintEquivClasses(mp, pdrgpcrs);

	pcnstrEq->Release();
	pexprEq->Release();

	pdrgpcrs->Release();

	pexprGet1->Release();
	pexprGet2->Release();

	return GPOS_OK;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CConstraintIntervalConvertsTo
//
//	@doc:
//		Tests CConstraintInterval::ConvertsToIn and
//		CConstraintInterval::ConvertsToNotIn
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CConstraintIntervalConvertsTo()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	// create a range which should convert to an IN array expression
	const SRangeInfo rgRangeInfoIn[] = {
		{CRange::EriIncluded, -1000, CRange::EriIncluded, -1000},
		{CRange::EriIncluded, -5, CRange::EriIncluded, -5},
		{CRange::EriIncluded, 0, CRange::EriIncluded, 0}};

	// metadata id
	IMDTypeInt8 *pmdtypeint8 =
		(IMDTypeInt8 *) mda.PtMDType<IMDTypeInt8>(CTestUtils::m_sysidDefault);
	IMDId *mdid = pmdtypeint8->MDId();

	// get a column ref
	CExpression *pexprGet = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs = pexprGet->DeriveOutputColumns();
	CColRef *colref = pcrs->PcrAny();

	// create constraint
	CRangeArray *pdrgprng =
		Pdrgprng(mp, mdid, rgRangeInfoIn, GPOS_ARRAY_SIZE(rgRangeInfoIn));
	CConstraintInterval *pcnstin =
		GPOS_NEW(mp) CConstraintInterval(mp, colref, pdrgprng, true);

	PrintConstraint(mp, pcnstin);

	// should convert to in
	GPOS_UNITTEST_ASSERT(pcnstin->FConvertsToIn());
	GPOS_UNITTEST_ASSERT(!pcnstin->FConvertsToNotIn());

	CConstraintInterval *pcnstNotIn = pcnstin->PciComplement(mp);

	// should convert to a not in statement after taking the complement
	GPOS_UNITTEST_ASSERT(pcnstNotIn->FConvertsToNotIn());
	GPOS_UNITTEST_ASSERT(!pcnstNotIn->FConvertsToIn());

	pcnstin->Release();
	pcnstNotIn->Release();
	pexprGet->Release();

	return GPOS_OK;
}


//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CConstraintIntervalPexpr
//
//	@doc:
//		Tests CConstraintInterval::PexprConstructArrayScalar
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CConstraintIntervalPexpr()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	CAutoTraceFlag atf(EopttraceArrayConstraints, true);

	// create a range which should convert to an IN array expression
	const SRangeInfo rgRangeInfoIn[] = {
		{CRange::EriIncluded, -1000, CRange::EriIncluded, -1000},
		{CRange::EriIncluded, -5, CRange::EriIncluded, -5},
		{CRange::EriIncluded, 0, CRange::EriIncluded, 0}};

	// metadata id
	IMDTypeInt8 *pmdtypeint8 =
		(IMDTypeInt8 *) mda.PtMDType<IMDTypeInt8>(CTestUtils::m_sysidDefault);
	IMDId *mdid = pmdtypeint8->MDId();

	// get a column ref
	CExpression *pexprGet = CTestUtils::PexprLogicalGet(mp);
	CColRefSet *pcrs = pexprGet->DeriveOutputColumns();
	CColRef *colref = pcrs->PcrAny();

	CRangeArray *pdrgprng = nullptr;
	CConstraintInterval *pcnstin = nullptr;
	CExpression *pexpr = nullptr;
	CConstraintInterval *pcnstNotIn = nullptr;

	// IN CONSTRAINT FOR SIMPLE INTERVAL (WITHOUT NULL)

	// create constraint
	pdrgprng =
		Pdrgprng(mp, mdid, rgRangeInfoIn, GPOS_ARRAY_SIZE(rgRangeInfoIn));
	pcnstin = GPOS_NEW(mp) CConstraintInterval(mp, colref, pdrgprng, false);

	pexpr = pcnstin->PexprScalar(mp);  // pexpr is owned by the constraint
	PrintConstraint(mp, pcnstin);

	GPOS_UNITTEST_ASSERT(!pcnstin->FConvertsToNotIn());
	GPOS_UNITTEST_ASSERT(pcnstin->FConvertsToIn());
	GPOS_UNITTEST_ASSERT(CUtils::FScalarArrayCmp(pexpr));
	GPOS_UNITTEST_ASSERT(
		3 == CUtils::UlCountOperator(pexpr, COperator::EopScalarConst));

	pcnstin->Release();


	// IN CONSTRAINT FOR SIMPLE INTERVAL WITH NULL

	// create constraint
	pdrgprng =
		Pdrgprng(mp, mdid, rgRangeInfoIn, GPOS_ARRAY_SIZE(rgRangeInfoIn));
	pcnstin = GPOS_NEW(mp) CConstraintInterval(mp, colref, pdrgprng, true);

	pexpr = pcnstin->PexprScalar(mp);  // pexpr is owned by the constraint
	PrintConstraint(mp, pcnstin);

	GPOS_UNITTEST_ASSERT(!pcnstin->FConvertsToNotIn());
	GPOS_UNITTEST_ASSERT(pcnstin->FConvertsToIn());
	GPOS_UNITTEST_ASSERT(
		CUtils::FScalarBoolOp(pexpr, CScalarBoolOp::EboolopOr));
	GPOS_UNITTEST_ASSERT(CUtils::FScalarArrayCmp((*pexpr)[0]));
	GPOS_UNITTEST_ASSERT(
		3 == CUtils::UlCountOperator((*pexpr)[0], COperator::EopScalarConst));
	GPOS_UNITTEST_ASSERT(CUtils::FScalarNullTest((*pexpr)[1]));

	pcnstin->Release();


	// NOT IN CONSTRAINT FOR SIMPLE INTERVAL WITHOUT NULL

	// create constraint
	pdrgprng =
		Pdrgprng(mp, mdid, rgRangeInfoIn, GPOS_ARRAY_SIZE(rgRangeInfoIn));
	pcnstin = GPOS_NEW(mp) CConstraintInterval(mp, colref, pdrgprng, true);

	pcnstNotIn = pcnstin->PciComplement(mp);
	pcnstin->Release();

	pexpr = pcnstNotIn->PexprScalar(mp);  // pexpr is owned by the constraint
	PrintConstraint(mp, pcnstNotIn);

	GPOS_UNITTEST_ASSERT(pcnstNotIn->FConvertsToNotIn());
	GPOS_UNITTEST_ASSERT(!pcnstNotIn->FConvertsToIn());
	GPOS_UNITTEST_ASSERT(CUtils::FScalarArrayCmp(pexpr));
	GPOS_UNITTEST_ASSERT(
		3 == CUtils::UlCountOperator(pexpr, COperator::EopScalarConst));

	pcnstNotIn->Release();


	// NOT IN CONSTRAINT FOR SIMPLE INTERVAL WITH NULL

	// create constraint
	pdrgprng =
		Pdrgprng(mp, mdid, rgRangeInfoIn, GPOS_ARRAY_SIZE(rgRangeInfoIn));
	pcnstin = GPOS_NEW(mp) CConstraintInterval(mp, colref, pdrgprng, false);

	pcnstNotIn = pcnstin->PciComplement(mp);
	pcnstin->Release();

	pexpr = pcnstNotIn->PexprScalar(mp);  // pexpr is owned by the constraint
	PrintConstraint(mp, pcnstNotIn);

	GPOS_UNITTEST_ASSERT(pcnstNotIn->FConvertsToNotIn());
	GPOS_UNITTEST_ASSERT(!pcnstNotIn->FConvertsToIn());
	GPOS_UNITTEST_ASSERT(
		CUtils::FScalarBoolOp(pexpr, CScalarBoolOp::EboolopOr));
	GPOS_UNITTEST_ASSERT(CUtils::FScalarArrayCmp((*pexpr)[0]));
	GPOS_UNITTEST_ASSERT(
		3 == CUtils::UlCountOperator(pexpr, COperator::EopScalarConst));
	GPOS_UNITTEST_ASSERT(CUtils::FScalarNullTest((*pexpr)[1]));

	pcnstNotIn->Release();


	pexprGet->Release();

	return GPOS_OK;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CConstraintIntervalFromArrayExpr
//
//	@doc:
//		Tests CConstraintInterval::PcnstrIntervalFromScalarArrayCmp
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CConstraintIntervalFromArrayExpr()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	CAutoTraceFlag atf(EopttraceArrayConstraints, true);

	// Create an IN array expression
	CExpression *pexpr = CTestUtils::PexprLogicalSelectArrayCmp(mp);
	// get a ref to the comparison column
	CColRef *colref = pexpr->DeriveOutputColumns()->PcrAny();

	// remove the array child
	CExpression *pexprArrayComp = (*pexpr->PdrgPexpr())[1];
	GPOS_UNITTEST_ASSERT(CUtils::FScalarArrayCmp(pexprArrayComp));

	CConstraintInterval *pcnstIn =
		CConstraintInterval::PciIntervalFromScalarExpr(mp, pexprArrayComp,
													   colref);
	GPOS_UNITTEST_ASSERT(CConstraint::EctInterval == pcnstIn->Ect());
	GPOS_UNITTEST_ASSERT(
		pcnstIn->Pdrgprng()->Size() ==
		CUtils::UlCountOperator(pexprArrayComp, COperator::EopScalarConst));

	pcnstIn->Release();
	pexpr->Release();

	// test a NOT IN expression

	CExpression *pexprNotIn = CTestUtils::PexprLogicalSelectArrayCmp(
		mp, CScalarArrayCmp::EarrcmpAll, IMDType::EcmptNEq);
	CExpression *pexprArrayNotInComp = (*pexprNotIn->PdrgPexpr())[1];
	CColRef *pcrNot = pexprNotIn->DeriveOutputColumns()->PcrAny();
	CConstraintInterval *pcnstNotIn =
		CConstraintInterval::PciIntervalFromScalarExpr(mp, pexprArrayNotInComp,
													   pcrNot);
	GPOS_UNITTEST_ASSERT(CConstraint::EctInterval == pcnstNotIn->Ect());
	// a NOT IN range array should have one more element than the expression array consts
	GPOS_UNITTEST_ASSERT(
		pcnstNotIn->Pdrgprng()->Size() ==
		1 + CUtils::UlCountOperator(pexprArrayNotInComp,
									COperator::EopScalarConst));

	pexprNotIn->Release();
	pcnstNotIn->Release();

	// create an IN expression with repeated values
	IntPtrArray *pdrgpi = GPOS_NEW(mp) IntPtrArray(mp);
	INT aiValsRepeat[] = {5, 1, 2, 5, 3, 4, 5};
	ULONG aiValsLength = sizeof(aiValsRepeat) / sizeof(INT);
	for (ULONG ul = 0; ul < aiValsLength; ul++)
	{
		pdrgpi->Append(GPOS_NEW(mp) INT(aiValsRepeat[ul]));
	}
	CExpression *pexprInRepeatsSelect = CTestUtils::PexprLogicalSelectArrayCmp(
		mp, CScalarArrayCmp::EarrcmpAny, IMDType::EcmptEq, pdrgpi);
	CColRef *pcrInRepeats =
		pexprInRepeatsSelect->DeriveOutputColumns()->PcrAny();
	CExpression *pexprArrayCmpRepeats = (*pexprInRepeatsSelect->PdrgPexpr())[1];
	// add 2 repeated values and one unique
	CConstraintInterval *pcnstInRepeats =
		CConstraintInterval::PciIntervalFromScalarExpr(mp, pexprArrayCmpRepeats,
													   pcrInRepeats);
	GPOS_UNITTEST_ASSERT(5 == pcnstInRepeats->Pdrgprng()->Size());
	pexprInRepeatsSelect->Release();
	pcnstInRepeats->Release();

	// create a NOT IN expression with repeated values
	CExpression *pexprNotInRepeatsSelect =
		CTestUtils::PexprLogicalSelectArrayCmp(mp, CScalarArrayCmp::EarrcmpAll,
											   IMDType::EcmptNEq, pdrgpi);
	CColRef *pcrNotInRepeats =
		pexprNotInRepeatsSelect->DeriveOutputColumns()->PcrAny();
	CExpression *pexprNotInArrayCmpRepeats =
		(*pexprNotInRepeatsSelect->PdrgPexpr())[1];
	CConstraintInterval *pcnstNotInRepeats =
		CConstraintInterval::PciIntervalFromScalarExpr(
			mp, pexprNotInArrayCmpRepeats, pcrNotInRepeats);
	// a total of 5 unique ScalarConsts in the expression will result in 6 ranges
	GPOS_UNITTEST_ASSERT(6 == pcnstNotInRepeats->Pdrgprng()->Size());
	pexprNotInRepeatsSelect->Release();
	pcnstNotInRepeats->Release();
	pdrgpi->Release();

	return GPOS_OK;
}


//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CConstraintIntervalFromArrayExprIncludesNull
//
//	@doc:
//		Tests CConstraintInterval::PcnstrIntervalFromScalarArrayCmp in cases
//		where NULL is in the scalar array expression
//
//---------------------------------------------------------------------------
GPOS_RESULT
EresUnittest_CConstraintIntervalFromArrayExprIncludesNull()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	CAutoTraceFlag atf(EopttraceArrayConstraints, true);

	// test for includes NULL
	// create an IN expression with repeated values
	IntPtrArray *pdrgpi = GPOS_NEW(mp) IntPtrArray(mp);
	INT rngiValues[] = {1, 2};
	ULONG ulValsLength = GPOS_ARRAY_SIZE(rngiValues);
	for (ULONG ul = 0; ul < ulValsLength; ul++)
	{
		pdrgpi->Append(GPOS_NEW(mp) INT(rngiValues[ul]));
	}
	CExpression *pexprIn = CTestUtils::PexprLogicalSelectArrayCmp(
		mp, CScalarArrayCmp::EarrcmpAny, IMDType::EcmptEq, pdrgpi);

	CExpression *pexprArrayChild = (*(*pexprIn)[1])[1];
	// create a int4 datum
	const IMDTypeInt4 *pmdtypeint4 = mda.PtMDType<IMDTypeInt4>();
	IDatumInt4 *pdatumNull = pmdtypeint4->CreateInt4Datum(mp, 0, true);

	CExpression *pexprConstNull = GPOS_NEW(mp)
		CExpression(mp, GPOS_NEW(mp) CScalarConst(mp, (IDatum *) pdatumNull));
	pexprArrayChild->PdrgPexpr()->Append(pexprConstNull);

	CColRef *colref = pexprIn->DeriveOutputColumns()->PcrAny();
	CConstraintInterval *pci = CConstraintInterval::PciIntervalFromScalarExpr(
		mp, (*pexprIn)[1], colref);
	GPOS_UNITTEST_ASSERT(!pci->FIncludesNull());
	GPOS_UNITTEST_ASSERT(2 == pci->Pdrgprng()->Size());
	pexprIn->Release();
	pci->Release();
	pdrgpi->Release();

	return GPOS_OK;
}

// "col <earrcmpt> (const array)" over colref. The values are int4 constants;
// a NULL constant is appended if fAddNull.
static CExpression *
PexprArrayCmpOnColRef(CMemoryPool *mp, CColRef *colref,
					  CScalarArrayCmp::EArrCmpType earrcmpt,
					  IMDType::ECmpType ecmpt, const INT *rgi, ULONG ulLen,
					  BOOL fAddNull = false)
{
	CExpressionArray *pdrgpexprElems = GPOS_NEW(mp) CExpressionArray(mp);
	for (ULONG ul = 0; ul < ulLen; ul++)
	{
		pdrgpexprElems->Append(CUtils::PexprScalarConstInt4(mp, rgi[ul]));
	}
	if (fAddNull)
	{
		CMDAccessor *mda = COptCtxt::PoctxtFromTLS()->Pmda();
		IDatumInt4 *pdatumNull =
			mda->PtMDType<IMDTypeInt4>()->CreateInt4Datum(mp, 0, true);
		pdrgpexprElems->Append(GPOS_NEW(mp) CExpression(
			mp, GPOS_NEW(mp) CScalarConst(mp, (IDatum *) pdatumNull)));
	}

	return CUtils::PexprScalarArrayCmp(mp, earrcmpt, ecmpt, pdrgpexprElems,
									   colref);
}

//---------------------------------------------------------------------------
//	@function:
//		EresUnittest_CConstraintIntervalArrayCache
//
//	@doc:
//		Tests the per-query cache of CConstraintInterval derived from
//		"col IN/NOT IN (const array)": a hit returns the very same object, the
//		key is the array CONTENT (an equal array built again hits, a different
//		array or infer_nulls_as does not), and the cache is off without the
//		trace flag.
//
//---------------------------------------------------------------------------
GPOS_RESULT
EresUnittest_CConstraintIntervalArrayCache()
{
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));

	CAutoTraceFlag atfArray(EopttraceArrayConstraints, true);
	CAutoTraceFlag atfCache(EopttraceArrayConstraintCache, true);

	CExpression *pexprGet = CTestUtils::PexprLogicalGet(mp);
	CColRef *colref = pexprGet->DeriveOutputColumns()->PcrAny();

	const INT rgiA[] = {5, 1, 3, 1};
	const INT rgiB[] = {5, 1, 3, 2};

	CExpression *pexprA1 = PexprArrayCmpOnColRef(
		mp, colref, CScalarArrayCmp::EarrcmpAny, IMDType::EcmptEq, rgiA, 4);
	CConstraintInterval *pci1 =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprA1,
															  colref, false);
	GPOS_UNITTEST_ASSERT(nullptr != pci1);
	GPOS_UNITTEST_ASSERT(3 == pci1->Pdrgprng()->Size());  // {1, 3, 5}

	// same expression again: the cache hands out the same object
	CConstraintInterval *pci2 =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprA1,
															  colref, false);
	GPOS_UNITTEST_ASSERT(pci1 == pci2);

	// an equal array that was translated again (new operator, new expression
	// objects, as the CHECK constraint path does) hits as well
	CExpression *pexprA2 = PexprArrayCmpOnColRef(
		mp, colref, CScalarArrayCmp::EarrcmpAny, IMDType::EcmptEq, rgiA, 4);
	GPOS_UNITTEST_ASSERT(pexprA1 != pexprA2);
	GPOS_UNITTEST_ASSERT(pexprA1->Pop() != pexprA2->Pop());
	CConstraintInterval *pci3 =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprA2,
															  colref, false);
	GPOS_UNITTEST_ASSERT(pci1 == pci3);

	// a different array must not hit, even though everything else is equal
	CExpression *pexprB = PexprArrayCmpOnColRef(
		mp, colref, CScalarArrayCmp::EarrcmpAny, IMDType::EcmptEq, rgiB, 4);
	CConstraintInterval *pci4 =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprB,
															  colref, false);
	GPOS_UNITTEST_ASSERT(nullptr != pci4 && pci1 != pci4);
	GPOS_UNITTEST_ASSERT(4 == pci4->Pdrgprng()->Size());  // {1, 2, 3, 5}

	// infer_nulls_as is part of the key
	CConstraintInterval *pci5 =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprA1,
															  colref, true);
	GPOS_UNITTEST_ASSERT(nullptr != pci5 && pci1 != pci5);
	GPOS_UNITTEST_ASSERT(!pci1->FIncludesNull() && pci5->FIncludesNull());

	// IN and NOT IN over the same values are different entries
	CExpression *pexprNotIn = PexprArrayCmpOnColRef(
		mp, colref, CScalarArrayCmp::EarrcmpAll, IMDType::EcmptNEq, rgiA, 4);
	CConstraintInterval *pci6 =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprNotIn,
															  colref, false);
	GPOS_UNITTEST_ASSERT(nullptr != pci6 && pci1 != pci6);
	GPOS_UNITTEST_ASSERT(4 == pci6->Pdrgprng()->Size());  // (-inf,1) (1,3) (3,5) (5,inf)

	// Arrays as the DXL translator produces them are collapsed: no children,
	// the constants live in the CScalarArray operator. Different collapsed
	// arrays must hash apart (the constants are part of the hash), and an
	// equal collapsed array must hit.
	CExpression *pexprA1c = CUtils::PexprCollapseConstArray(mp, pexprA1);
	CExpression *pexprA2c = CUtils::PexprCollapseConstArray(mp, pexprA2);
	CExpression *pexprBc = CUtils::PexprCollapseConstArray(mp, pexprB);
	CExpression *pexprArrA1c = CUtils::PexprScalarArrayChild(pexprA1c);
	CExpression *pexprArrBc = CUtils::PexprScalarArrayChild(pexprBc);
	GPOS_UNITTEST_ASSERT(CUtils::FScalarArrayCollapsed(pexprArrA1c));
	GPOS_UNITTEST_ASSERT(0 == pexprArrA1c->Arity());
	{
		IMDId *mdid_op = CScalarArrayCmp::PopConvert(pexprA1c->Pop())->MdIdOp();
		const ULONG earrcmpt = static_cast<ULONG>(
			CScalarArrayCmp::PopConvert(pexprA1c->Pop())->Earrcmpt());
		SArrayCnstrCacheKey keyA(pexprArrA1c, mdid_op, earrcmpt, colref, false);
		SArrayCnstrCacheKey keyB(pexprArrBc, mdid_op, earrcmpt, colref, false);
		GPOS_UNITTEST_ASSERT(SArrayCnstrCacheKey::HashValue(&keyA) !=
							 SArrayCnstrCacheKey::HashValue(&keyB));
		GPOS_UNITTEST_ASSERT(!SArrayCnstrCacheKey::Equals(&keyA, &keyB));
	}
	CConstraintInterval *pciC1 =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprA1c,
															  colref, false);
	CConstraintInterval *pciC2 =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprA2c,
															  colref, false);
	CConstraintInterval *pciC3 =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprBc,
															  colref, false);
	GPOS_UNITTEST_ASSERT(nullptr != pciC1 && pciC1 == pciC2);
	GPOS_UNITTEST_ASSERT(nullptr != pciC3 && pciC1 != pciC3);
	GPOS_UNITTEST_ASSERT(3 == pciC1->Pdrgprng()->Size());
	GPOS_UNITTEST_ASSERT(4 == pciC3->Pdrgprng()->Size());
	pciC1->Release();
	pciC2->Release();
	pciC3->Release();
	pexprA1c->Release();
	pexprA2c->Release();
	pexprBc->Release();

	pci1->Release();
	pci2->Release();
	pci3->Release();
	pci4->Release();
	pci5->Release();
	pci6->Release();

	// without the cache every call builds a new interval
	{
		CAutoTraceFlag atfOff(EopttraceArrayConstraintCache, false);
		CConstraintInterval *pciN1 =
			CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(
				mp, pexprA1, colref, false);
		CConstraintInterval *pciN2 =
			CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(
				mp, pexprA1, colref, false);
		GPOS_UNITTEST_ASSERT(nullptr != pciN1 && pciN1 != pciN2);
		pciN1->Release();
		pciN2->Release();
	}

	pexprA1->Release();
	pexprA2->Release();
	pexprB->Release();
	pexprNotIn->Release();
	pexprGet->Release();

	return GPOS_OK;
}

//---------------------------------------------------------------------------
//	@function:
//		EresUnittest_CConstraintIntervalArrayThreshold
//
//	@doc:
//		Tests optimizer_array_interval_threshold and the NULL semantics of
//		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp:
//		  - an array above the threshold derives nothing and is counted;
//		  - NOT does not complement a child derived without such an array;
//		  - an array with a NULL constant is never skipped;
//		  - NULL constants follow SQL semantics under infer_nulls_as;
//		  - NOT IN of only NULLs does not crash.
//
//---------------------------------------------------------------------------
GPOS_RESULT
EresUnittest_CConstraintIntervalArrayThreshold()
{
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// optimizer config with optimizer_array_interval_threshold = 3
	COptimizerConfig *optimizer_config = GPOS_NEW(mp) COptimizerConfig(
		CEnumeratorConfig::GetEnumeratorCfg(mp, 0 /*plan_id*/),
		CStatisticsConfig::PstatsconfDefault(mp),
		CCTEConfig::PcteconfDefault(mp), CTestUtils::GetCostModel(mp),
		GPOS_NEW(mp) CHint(
			gpos::int_max /*join_arity_for_associativity_commutativity*/,
			gpos::int_max /*array_expansion_threshold*/, 3 /*array_interval*/,
			JOIN_ORDER_DP_THRESHOLD, BROADCAST_THRESHOLD,
			true /*enforce_constraint_on_dml*/,
			PUSH_GROUP_BY_BELOW_SETOP_THRESHOLD, XFORM_BIND_THRESHOLD,
			SKEW_FACTOR),
		nullptr /*pplanhint*/, CWindowOids::GetWindowOids(mp));
	CAutoOptCtxt aoc(mp, &mda, pceeval, optimizer_config);
	COptCtxt *poctxt = COptCtxt::PoctxtFromTLS();

	CAutoTraceFlag atfArray(EopttraceArrayConstraints, true);
	CAutoTraceFlag atfCache(EopttraceArrayConstraintCache, true);

	CExpression *pexprGet = CTestUtils::PexprLogicalGet(mp);
	CColRef *colref = pexprGet->DeriveOutputColumns()->PcrAny();

	const INT rgi3[] = {1, 2, 3};
	const INT rgi5[] = {1, 2, 3, 4, 5};

	// at the threshold: derived
	CExpression *pexpr3 = PexprArrayCmpOnColRef(
		mp, colref, CScalarArrayCmp::EarrcmpAny, IMDType::EcmptEq, rgi3, 3);
	ULONG ulBailouts = poctxt->UlArrayCnstrBailouts();
	CConstraintInterval *pci =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexpr3,
															  colref, false);
	GPOS_UNITTEST_ASSERT(nullptr != pci);
	GPOS_UNITTEST_ASSERT(ulBailouts == poctxt->UlArrayCnstrBailouts());
	pci->Release();

	// above the threshold: nothing derived, and it is counted
	CExpression *pexpr5 = PexprArrayCmpOnColRef(
		mp, colref, CScalarArrayCmp::EarrcmpAny, IMDType::EcmptEq, rgi5, 5);
	GPOS_UNITTEST_ASSERT(
		nullptr == CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(
					   mp, pexpr5, colref, false));
	GPOS_UNITTEST_ASSERT(ulBailouts + 1 == poctxt->UlArrayCnstrBailouts());

	// "col IS NOT NULL AND col IN (5 values)": AND keeps what it can derive
	pexpr5->AddRef();
	CExpressionArray *pdrgpexprAnd = GPOS_NEW(mp) CExpressionArray(mp);
	pdrgpexprAnd->Append(
		CUtils::PexprIsNotNull(mp, CUtils::PexprScalarIdent(mp, colref)));
	pdrgpexprAnd->Append(pexpr5);
	CExpression *pexprAnd =
		CUtils::PexprScalarBoolOp(mp, CScalarBoolOp::EboolopAnd, pdrgpexprAnd);
	CConstraintInterval *pciAnd =
		CConstraintInterval::PciIntervalFromScalarExpr(mp, pexprAnd, colref);
	GPOS_UNITTEST_ASSERT(nullptr != pciAnd);
	pciAnd->Release();

	// ... but NOT of it must not complement that partial result (this is the
	// constraint of a LIST DEFAULT partition; complementing would claim the
	// partition holds only NULL and prune it wrongly)
	pexprAnd->AddRef();
	CExpressionArray *pdrgpexprNot = GPOS_NEW(mp) CExpressionArray(mp);
	pdrgpexprNot->Append(pexprAnd);
	CExpression *pexprNot =
		CUtils::PexprScalarBoolOp(mp, CScalarBoolOp::EboolopNot, pdrgpexprNot);
	GPOS_UNITTEST_ASSERT(nullptr == CConstraintInterval::PciIntervalFromScalarExpr(
										mp, pexprNot, colref));

	// a NOT that does not contain a skipped list is complemented as before
	pexpr3->AddRef();
	CExpressionArray *pdrgpexprNot3 = GPOS_NEW(mp) CExpressionArray(mp);
	pdrgpexprNot3->Append(pexpr3);
	CExpression *pexprNot3 = CUtils::PexprScalarBoolOp(
		mp, CScalarBoolOp::EboolopNot, pdrgpexprNot3);
	CConstraintInterval *pciNot3 =
		CConstraintInterval::PciIntervalFromScalarExpr(mp, pexprNot3, colref);
	GPOS_UNITTEST_ASSERT(nullptr != pciNot3);
	pciNot3->Release();

	// an array with a NULL constant is never skipped, whatever its size
	CExpression *pexprNull5 =
		PexprArrayCmpOnColRef(mp, colref, CScalarArrayCmp::EarrcmpAny,
							  IMDType::EcmptEq, rgi5, 5, true /*fAddNull*/);
	ulBailouts = poctxt->UlArrayCnstrBailouts();
	CConstraintInterval *pciNull =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprNull5,
															  colref, false);
	GPOS_UNITTEST_ASSERT(nullptr != pciNull);
	GPOS_UNITTEST_ASSERT(ulBailouts == poctxt->UlArrayCnstrBailouts());
	GPOS_UNITTEST_ASSERT(5 == pciNull->Pdrgprng()->Size());	 // NULL dropped
	GPOS_UNITTEST_ASSERT(!pciNull->FIncludesNull());
	pciNull->Release();

	// CHECK semantics (infer_nulls_as): x IN (..., NULL) is NULL for every
	// non-member, which passes, so every value and NULL pass
	CConstraintInterval *pciCheck =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(mp, pexprNull5,
															  colref, true);
	GPOS_UNITTEST_ASSERT(nullptr != pciCheck);
	GPOS_UNITTEST_ASSERT(pciCheck->IsConstraintUnbounded());
	pciCheck->Release();

	// NOT IN (NULL, NULL): never TRUE (used to dereference a null pointer)
	CExpression *pexprNotInNulls = CUtils::PexprScalarArrayCmp(
		mp, CScalarArrayCmp::EarrcmpAll, IMDType::EcmptNEq,
		[&]() {
			CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
			for (ULONG ul = 0; ul < 2; ul++)
			{
				IDatumInt4 *pdatumNull =
					mda.PtMDType<IMDTypeInt4>()->CreateInt4Datum(mp, 0, true);
				pdrgpexpr->Append(GPOS_NEW(mp) CExpression(
					mp, GPOS_NEW(mp) CScalarConst(mp, (IDatum *) pdatumNull)));
			}
			return pdrgpexpr;
		}(),
		colref);
	CConstraintInterval *pciNotInNulls =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(
			mp, pexprNotInNulls, colref, false);
	GPOS_UNITTEST_ASSERT(nullptr != pciNotInNulls);
	GPOS_UNITTEST_ASSERT(pciNotInNulls->FContradiction());
	pciNotInNulls->Release();
	CConstraintInterval *pciNotInNullsCheck =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(
			mp, pexprNotInNulls, colref, true);
	GPOS_UNITTEST_ASSERT(nullptr != pciNotInNullsCheck);
	GPOS_UNITTEST_ASSERT(pciNotInNullsCheck->IsConstraintUnbounded());
	pciNotInNullsCheck->Release();

	// NOT IN (1, 2, 3, NULL): never TRUE in a WHERE clause (x <> NULL is
	// NULL), so the exact interval is a contradiction. It used to be the
	// superset (-inf,1) (1,2) (2,3) (3,inf), whose complement under a NOT
	// (CHECK (NOT (c NOT IN (...)))) wrongly excluded every other value.
	CExpression *pexprNotInWithNull =
		PexprArrayCmpOnColRef(mp, colref, CScalarArrayCmp::EarrcmpAll,
							  IMDType::EcmptNEq, rgi3, 3, true /*fAddNull*/);
	CConstraintInterval *pciNotInWithNull =
		CConstraintInterval::PcnstrIntervalFromScalarArrayCmp(
			mp, pexprNotInWithNull, colref, false);
	GPOS_UNITTEST_ASSERT(nullptr != pciNotInWithNull);
	GPOS_UNITTEST_ASSERT(pciNotInWithNull->FContradiction());
	pciNotInWithNull->Release();

	pexprNotInWithNull->AddRef();
	CExpressionArray *pdrgpexprNotNotIn = GPOS_NEW(mp) CExpressionArray(mp);
	pdrgpexprNotNotIn->Append(pexprNotInWithNull);
	CExpression *pexprNotNotIn = CUtils::PexprScalarBoolOp(
		mp, CScalarBoolOp::EboolopNot, pdrgpexprNotNotIn);
	// CHECK semantics: NOT (c NOT IN (1, 2, 3, NULL)) is NULL for every c
	// outside the list, and NULL passes a CHECK, so every value passes
	CConstraintInterval *pciNotNotIn =
		CConstraintInterval::PciIntervalFromScalarExpr(mp, pexprNotNotIn,
													   colref, true);
	GPOS_UNITTEST_ASSERT(nullptr != pciNotNotIn);
	GPOS_UNITTEST_ASSERT(pciNotNotIn->IsConstraintUnbounded());
	pciNotNotIn->Release();
	pexprNotNotIn->Release();
	pexprNotInWithNull->Release();

	pexprNotInNulls->Release();
	pexprNull5->Release();
	pexprNot3->Release();
	pexprNot->Release();
	pexprAnd->Release();
	pexpr5->Release();
	pexpr3->Release();
	pexprGet->Release();

	return GPOS_OK;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CIntervalFromScalarCmp
//
//	@doc:
//		Interval from scalar comparison
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CIntervalFromScalarCmp(CMemoryPool *mp,
													 CMDAccessor *md_accessor,
													 CColRef *colref)
{
	IMDType::ECmpType rgecmpt[] = {
		IMDType::EcmptEq,  IMDType::EcmptNEq, IMDType::EcmptL,
		IMDType::EcmptLEq, IMDType::EcmptG,	  IMDType::EcmptGEq,
	};

	for (ULONG ul = 0; ul < GPOS_ARRAY_SIZE(rgecmpt); ul++)
	{
		CExpression *pexprScCmp =
			PexprScalarCmp(mp, md_accessor, colref, rgecmpt[ul], 4);
		CConstraintInterval *pci =
			CConstraintInterval::PciIntervalFromScalarExpr(mp, pexprScCmp,
														   colref);
		PrintConstraint(mp, pci);

		pci->Release();
		pexprScCmp->Release();
	}

	return GPOS_OK;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_CIntervalFromScalarBoolOp
//
//	@doc:
//		Interval from scalar bool op
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_CIntervalFromScalarBoolOp(
	CMemoryPool *mp, CMDAccessor *md_accessor, CColRef *colref)
{
	// AND
	CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
	pdrgpexpr->Append(
		PexprScalarCmp(mp, md_accessor, colref, IMDType::EcmptL, 5));
	pdrgpexpr->Append(
		PexprScalarCmp(mp, md_accessor, colref, IMDType::EcmptGEq, 0));

	CExpression *pexpr =
		CUtils::PexprScalarBoolOp(mp, CScalarBoolOp::EboolopAnd, pdrgpexpr);
	(void) pexpr->PdpDerive();

	CConstraintInterval *pciAnd =
		CConstraintInterval::PciIntervalFromScalarExpr(mp, pexpr, colref);
	PrintConstraint(mp, pciAnd);

	pciAnd->Release();
	(void) pexpr->Release();

	// OR
	pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
	pdrgpexpr->Append(
		PexprScalarCmp(mp, md_accessor, colref, IMDType::EcmptL, 5));
	pdrgpexpr->Append(
		PexprScalarCmp(mp, md_accessor, colref, IMDType::EcmptGEq, 10));

	pexpr = CUtils::PexprScalarBoolOp(mp, CScalarBoolOp::EboolopOr, pdrgpexpr);
	(void) pexpr->PdpDerive();

	CConstraintInterval *pciOr =
		CConstraintInterval::PciIntervalFromScalarExpr(mp, pexpr, colref);
	PrintConstraint(mp, pciOr);

	pciOr->Release();
	pexpr->Release();

	// NOT
	pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
	pdrgpexpr->Append(
		CUtils::PexprIsNull(mp, CUtils::PexprScalarIdent(mp, colref)));

	pexpr = CUtils::PexprScalarBoolOp(mp, CScalarBoolOp::EboolopNot, pdrgpexpr);
	(void) pexpr->PdpDerive();

	CConstraintInterval *pciNot =
		CConstraintInterval::PciIntervalFromScalarExpr(mp, pexpr, colref);
	PrintConstraint(mp, pciNot);

	pciNot->Release();
	pexpr->Release();

	return GPOS_OK;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::PexprScalarCmp
//
//	@doc:
//		Generate comparison expression
//
//---------------------------------------------------------------------------
CExpression *
CConstraintTest::PexprScalarCmp(CMemoryPool *mp, CMDAccessor *md_accessor,
								CColRef *colref, IMDType::ECmpType cmp_type,
								LINT val)
{
	CExpression *pexprConst = CUtils::PexprScalarConstInt8(mp, val);

	const IMDTypeInt8 *pmdtypeint8 = md_accessor->PtMDType<IMDTypeInt8>();
	IMDId *mdid_op = pmdtypeint8->GetMdidForCmpType(cmp_type);
	mdid_op->AddRef();

	const CMDName mdname = md_accessor->RetrieveScOp(mdid_op)->Mdname();

	CWStringConst strOpName(mdname.GetMDName()->GetBuffer());

	CExpression *pexpr =
		CUtils::PexprScalarCmp(mp, colref, pexprConst, strOpName, mdid_op);
	(void) pexpr->PdpDerive();
	return pexpr;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::PciFirstInterval
//
//	@doc:
//		Create an interval
//
//---------------------------------------------------------------------------
CConstraintInterval *
CConstraintTest::PciFirstInterval(CMemoryPool *mp, IMDId *mdid, CColRef *colref)
{
	const SRangeInfo rgRangeInfo[] = {
		{CRange::EriExcluded, -1000, CRange::EriIncluded, -20},
		{CRange::EriExcluded, -15, CRange::EriExcluded, -5},
		{CRange::EriIncluded, 0, CRange::EriExcluded, 5},
		{CRange::EriIncluded, 10, CRange::EriIncluded, 10},
		{CRange::EriExcluded, 20, CRange::EriExcluded, 1000},
	};

	CRangeArray *pdrgprng =
		Pdrgprng(mp, mdid, rgRangeInfo, GPOS_ARRAY_SIZE(rgRangeInfo));

	return GPOS_NEW(mp)
		CConstraintInterval(mp, colref, pdrgprng, true /*is_null*/);
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::PciSecondInterval
//
//	@doc:
//		Create an interval
//
//---------------------------------------------------------------------------
CConstraintInterval *
CConstraintTest::PciSecondInterval(CMemoryPool *mp, IMDId *mdid,
								   CColRef *colref)
{
	const SRangeInfo rgRangeInfo[] = {
		{CRange::EriExcluded, -30, CRange::EriExcluded, 1},
		{CRange::EriIncluded, 3, CRange::EriIncluded, 3},
		{CRange::EriExcluded, 10, CRange::EriExcluded, 25},
	};

	CRangeArray *pdrgprng =
		Pdrgprng(mp, mdid, rgRangeInfo, GPOS_ARRAY_SIZE(rgRangeInfo));

	return GPOS_NEW(mp)
		CConstraintInterval(mp, colref, pdrgprng, false /*is_null*/);
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::Pdrgprng
//
//	@doc:
//		Construct an array of ranges to be used to create an interval
//
//---------------------------------------------------------------------------
CRangeArray *
CConstraintTest::Pdrgprng(CMemoryPool *mp, IMDId *mdid,
						  const SRangeInfo rgRangeInfo[], ULONG ulRanges)
{
	CRangeArray *pdrgprng = GPOS_NEW(mp) CRangeArray(mp);

	for (ULONG ul = 0; ul < ulRanges; ul++)
	{
		SRangeInfo rnginfo = rgRangeInfo[ul];
		mdid->AddRef();
		CRange *prange = GPOS_NEW(mp)
			CRange(mdid, COptCtxt::PoctxtFromTLS()->Pcomp(),
				   GPOS_NEW(mp) CDatumInt8GPDB(CTestUtils::m_sysidDefault,
											   (LINT) rnginfo.iLeft),
				   rnginfo.eriLeft,
				   GPOS_NEW(mp) CDatumInt8GPDB(CTestUtils::m_sysidDefault,
											   (LINT) rnginfo.iRight),
				   rnginfo.eriRight);
		pdrgprng->Append(prange);
	}

	return pdrgprng;
}


//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::PrintConstraint
//
//	@doc:
//		Print a constraint and its expression representation
//
//---------------------------------------------------------------------------
void
CConstraintTest::PrintConstraint(CMemoryPool *mp, CConstraint *pcnstr)
{
	CExpression *pexpr = pcnstr->PexprScalar(mp);

	// debug print
	CAutoTrace at(mp);
	at.Os() << std::endl;
	at.Os() << "CONSTRAINT:" << std::endl
			<< *pcnstr << std::endl
			<< "EXPR:" << std::endl
			<< *pexpr << std::endl;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::PrintEquivClasses
//
//	@doc:
//		Print equivalent classes
//
//---------------------------------------------------------------------------
void
CConstraintTest::PrintEquivClasses(CMemoryPool *mp, CColRefSetArray *pdrgpcrs,
								   BOOL fExpected)
{
	// debug print
	CAutoTrace at(mp);
	at.Os() << std::endl;
	if (fExpected)
	{
		at.Os() << "EXPECTED ";
	}
	else
	{
		at.Os() << "ACTUAL ";
	}
	at.Os() << "EQUIVALENCE CLASSES: [ ";

	if (nullptr == pdrgpcrs)
	{
		at.Os() << "]" << std::endl;

		return;
	}

	for (ULONG ul = 0; ul < pdrgpcrs->Size(); ul++)
	{
		at.Os() << "{" << *(*pdrgpcrs)[ul] << "} ";
	}

	at.Os() << "]" << std::endl;
}

#ifdef GPOS_DEBUG
//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_NegativeTests
//
//	@doc:
//		Tests for unconstrainable types.
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_NegativeTests()
{
	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();

	// we need to use an auto pointer for the cache here to ensure
	// deleting memory of cached objects when we throw
	CAutoP<CMDAccessor::MDCache> apcache;
	apcache =
		CCacheFactory::CreateCache<gpopt::IMDCacheObject *, gpopt::CMDKey *>(
			true,  // fUnique
			0 /* unlimited cache quota */, CMDKey::UlHashMDKey,
			CMDKey::FEqualMDKey);

	CMDAccessor::MDCache *pcache = apcache.Value();

	CMDAccessor mda(mp, pcache, CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	const IMDType *pmdtype = mda.RetrieveType(&CMDIdGPDB::m_mdid_text);
	CWStringConst str(GPOS_WSZ_LIT("text_col"));
	CName name(mp, &str);
	CAutoP<CColRef> colref(COptCtxt::PoctxtFromTLS()->Pcf()->PcrCreate(
		pmdtype, default_type_modifier, name));

	// create a text interval: ['baz', 'foobar')
	CAutoP<CWStringDynamic> pstrLower1(
		GPOS_NEW(mp) CWStringDynamic(mp, GPOS_WSZ_LIT("AAAAB2Jheg==")));
	CAutoP<CWStringDynamic> pstrUpper1(
		GPOS_NEW(mp) CWStringDynamic(mp, GPOS_WSZ_LIT("AAAACmZvb2Jhcg==")));
	const LINT lLower1 = 571163436;
	const LINT lUpper1 = 322061118;

	// 'text' is not a constrainable type, so the interval construction should assert-fail
	CConstraintInterval *pciFirst = CTestUtils::PciGenericInterval(
		mp, &mda, CMDIdGPDB::m_mdid_text, colref.Value(), pstrLower1.Value(),
		lLower1, CRange::EriIncluded, pstrUpper1.Value(), lUpper1,
		CRange::EriExcluded);
	PrintConstraint(mp, pciFirst);
	pciFirst->Release();

	return GPOS_OK;
}
#endif	// GPOS_DEBUG

//---------------------------------------------------------------------------
//	@function:
//		CConstraintTest::EresUnittest_ConstraintsOnDates
//	@doc:
//		Test constraints on date intervals.
//
//---------------------------------------------------------------------------
GPOS_RESULT
CConstraintTest::EresUnittest_ConstraintsOnDates()
{
	CAutoTraceFlag atf(EopttraceEnableConstantExpressionEvaluation,
					   true /*value*/);

	// create memory pool
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	// setup a file-based provider
	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache(), CTestUtils::m_sysidDefault, pmdp);

	CConstExprEvaluatorForDates *pceeval =
		GPOS_NEW(mp) CConstExprEvaluatorForDates(mp);

	// install opt context in TLS
	CAutoOptCtxt aoc(mp, &mda, pceeval, CTestUtils::GetCostModel(mp));
	GPOS_UNITTEST_ASSERT(nullptr != COptCtxt::PoctxtFromTLS()->Pcomp());

	const IMDType *pmdtype = mda.RetrieveType(&CMDIdGPDB::m_mdid_date);
	CWStringConst str(GPOS_WSZ_LIT("date_col"));
	CName name(mp, &str);
	CAutoP<CColRef> colref(COptCtxt::PoctxtFromTLS()->Pcf()->PcrCreate(
		pmdtype, default_type_modifier, name));

	// create a date interval: ['01-01-2012', '01-21-2012')
	CWStringDynamic pstrLowerDate1(mp, wszInternalRepresentationFor2012_01_01);
	CWStringDynamic pstrUpperDate1(mp, wszInternalRepresentationFor2012_01_21);
	CConstraintInterval *pciFirst = CTestUtils::PciGenericInterval(
		mp, &mda, CMDIdGPDB::m_mdid_date, colref.Value(), &pstrLowerDate1,
		lInternalRepresentationFor2012_01_01, CRange::EriIncluded,
		&pstrUpperDate1, lInternalRepresentationFor2012_01_21,
		CRange::EriExcluded);
	PrintConstraint(mp, pciFirst);

	// create a date interval: ['01-02-2012', '01-22-2012')
	CWStringDynamic pstrLowerDate2(mp, wszInternalRepresentationFor2012_01_02);
	CWStringDynamic pstrUpperDate2(mp, wszInternalRepresentationFor2012_01_22);
	CConstraintInterval *pciSecond = CTestUtils::PciGenericInterval(
		mp, &mda, CMDIdGPDB::m_mdid_date, colref.Value(), &pstrLowerDate2,
		lInternalRepresentationFor2012_01_02, CRange::EriIncluded,
		&pstrUpperDate2, lInternalRepresentationFor2012_01_22,
		CRange::EriExcluded);
	PrintConstraint(mp, pciSecond);

	// intersection
	CConstraintInterval *pciIntersect = pciFirst->PciIntersect(mp, pciSecond);
	PrintConstraint(mp, pciIntersect);
	CConstraintInterval *pciIntersectExpected = CTestUtils::PciGenericInterval(
		mp, &mda, CMDIdGPDB::m_mdid_date, colref.Value(), &pstrLowerDate2,
		lInternalRepresentationFor2012_01_02, CRange::EriIncluded,
		&pstrUpperDate1, lInternalRepresentationFor2012_01_21,
		CRange::EriExcluded);
	GPOS_UNITTEST_ASSERT(pciIntersectExpected->Equals(pciIntersect));

	// union
	CConstraintInterval *pciUnion = pciFirst->PciUnion(mp, pciSecond);
	PrintConstraint(mp, pciUnion);
	CConstraintInterval *pciUnionExpected = CTestUtils::PciGenericInterval(
		mp, &mda, CMDIdGPDB::m_mdid_date, colref.Value(), &pstrLowerDate1,
		lInternalRepresentationFor2012_01_01, CRange::EriIncluded,
		&pstrUpperDate2, lInternalRepresentationFor2012_01_22,
		CRange::EriExcluded);
	GPOS_UNITTEST_ASSERT(pciUnionExpected->Equals(pciUnion));

	// difference between pciFirst and pciSecond
	CConstraintInterval *pciDiff1 = pciFirst->PciDifference(mp, pciSecond);
	PrintConstraint(mp, pciDiff1);
	CConstraintInterval *pciDiff1Expected = CTestUtils::PciGenericInterval(
		mp, &mda, CMDIdGPDB::m_mdid_date, colref.Value(), &pstrLowerDate1,
		lInternalRepresentationFor2012_01_01, CRange::EriIncluded,
		&pstrLowerDate2, lInternalRepresentationFor2012_01_02,
		CRange::EriExcluded);
	GPOS_UNITTEST_ASSERT(pciDiff1Expected->Equals(pciDiff1));

	// difference between pciSecond and pciFirst
	CConstraintInterval *pciDiff2 = pciSecond->PciDifference(mp, pciFirst);
	PrintConstraint(mp, pciDiff2);
	CConstraintInterval *pciDiff2Expected = CTestUtils::PciGenericInterval(
		mp, &mda, CMDIdGPDB::m_mdid_date, colref.Value(), &pstrUpperDate1,
		lInternalRepresentationFor2012_01_21, CRange::EriIncluded,
		&pstrUpperDate2, lInternalRepresentationFor2012_01_22,
		CRange::EriExcluded);
	GPOS_UNITTEST_ASSERT(pciDiff2Expected->Equals(pciDiff2));

	// containment
	GPOS_UNITTEST_ASSERT(!pciFirst->Contains(pciSecond));
	GPOS_UNITTEST_ASSERT(pciFirst->Contains(pciDiff1));
	GPOS_UNITTEST_ASSERT(!pciSecond->Contains(pciFirst));
	GPOS_UNITTEST_ASSERT(pciSecond->Contains(pciDiff2));
	GPOS_UNITTEST_ASSERT(pciFirst->Contains(pciFirst));
	GPOS_UNITTEST_ASSERT(pciSecond->Contains(pciSecond));

	// equality
	// create a third interval identical to the first
	CConstraintInterval *pciThird = CTestUtils::PciGenericInterval(
		mp, &mda, CMDIdGPDB::m_mdid_date, colref.Value(), &pstrLowerDate1,
		lInternalRepresentationFor2012_01_01, CRange::EriIncluded,
		&pstrUpperDate1, lInternalRepresentationFor2012_01_21,
		CRange::EriExcluded);
	GPOS_UNITTEST_ASSERT(!pciFirst->Equals(pciSecond));
	GPOS_UNITTEST_ASSERT(!pciFirst->Equals(pciDiff1));
	GPOS_UNITTEST_ASSERT(!pciSecond->Equals(pciDiff2));
	GPOS_UNITTEST_ASSERT(pciFirst->Equals(pciThird));

	pciThird->Release();
	pciDiff2Expected->Release();
	pciDiff1Expected->Release();
	pciUnionExpected->Release();
	pciIntersectExpected->Release();
	pciDiff2->Release();
	pciDiff1->Release();
	pciUnion->Release();
	pciIntersect->Release();
	pciSecond->Release();
	pciFirst->Release();

	return GPOS_OK;
}

// EOF
