//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2008, 2009 Greenplum, Inc.
//
//	@filename:
//		CColumnFactoryTest.cpp
//
//	@doc:
//		Test for CColumnFactory
//---------------------------------------------------------------------------

#include "unittest/gpopt/base/CColumnFactoryTest.h"

#include "gpos/base.h"
#include "gpos/memory/CAutoMemoryPool.h"
#include "gpos/test/CUnittest.h"

#include "gpopt/base/CColumnFactory.h"
#include "gpopt/base/COptCtxt.h"
#include "gpopt/base/CQueryContext.h"
#include "gpopt/mdcache/CMDAccessor.h"
#include "gpopt/mdcache/CMDCache.h"
#include "naucrates/md/CMDProviderMemory.h"
#include "naucrates/md/IMDTypeInt4.h"

#include "unittest/gpopt/CTestUtils.h"
#include "unittest/gpopt/translate/CTranslatorExprToDXLTest.h"

using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CColumnFactoryTest::EresUnittest
//
//	@doc:
//		Unittest for column factory
//
//---------------------------------------------------------------------------
GPOS_RESULT
CColumnFactoryTest::EresUnittest()
{
	CUnittest rgut[] = {
		GPOS_UNITTEST_FUNC(CColumnFactoryTest::EresUnittest_Basic),
		GPOS_UNITTEST_FUNC(CColumnFactoryTest::EresUnittest_PcrCopyPreservesUsage)};

	return CUnittest::EresExecute(rgut, GPOS_ARRAY_SIZE(rgut));
}


//---------------------------------------------------------------------------
//	@function:
//		CColumnFactoryTest::EresUnittest_Basic
//
//	@doc:
//		Basic array allocation test
//
//---------------------------------------------------------------------------
GPOS_RESULT
CColumnFactoryTest::EresUnittest_Basic()
{
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache());
	mda.RegisterProvider(CTestUtils::m_sysidDefault, pmdp);

	const IMDTypeInt4 *pmdtypeint4 = mda.PtMDType<IMDTypeInt4>();

	CColumnFactory cf;

	// typed colref
	CColRef *pcrOne = cf.PcrCreate(pmdtypeint4, default_type_modifier);
	GPOS_UNITTEST_ASSERT(pcrOne == cf.LookupColRef(pcrOne->m_id));
	cf.Destroy(pcrOne);

	// typed/named colref
	CWStringConst strName(GPOS_WSZ_LIT("C_CustKey"));
	CColRef *pcrTwo =
		cf.PcrCreate(pmdtypeint4, default_type_modifier, CName(&strName));
	GPOS_UNITTEST_ASSERT(pcrTwo == cf.LookupColRef(pcrTwo->m_id));

	// clone previous colref
	CColRef *pcrThree = cf.PcrCreate(pcrTwo);
	GPOS_UNITTEST_ASSERT(pcrThree != cf.LookupColRef(pcrTwo->m_id));
	GPOS_UNITTEST_ASSERT(!pcrThree->Name().Equals(pcrTwo->Name()));
	cf.Destroy(pcrThree);

	cf.Destroy(pcrTwo);

	return GPOS_OK;
}


//---------------------------------------------------------------------------
//	@function:
//		CColumnFactoryTest::EresUnittest_PcrCopyPreservesUsage
//
//	@doc:
//		Verify that PcrCopy preserves the source colref's EUsedStatus
//		tristate (EUsed / EUnused / EUnknown). Regression guard against
//		re-introduction of the BOOL collapse: previously, an EUnused
//		source colref was copied as EUnknown because PcrCopy passed a
//		BOOL through PcrCreate.
//
//---------------------------------------------------------------------------
GPOS_RESULT
CColumnFactoryTest::EresUnittest_PcrCopyPreservesUsage()
{
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	CMDProviderMemory *pmdp = CTestUtils::m_pmdpf;
	pmdp->AddRef();
	CMDAccessor mda(mp, CMDCache::Pcache());
	mda.RegisterProvider(CTestUtils::m_sysidDefault, pmdp);

	const IMDTypeInt4 *pmdtypeint4 = mda.PtMDType<IMDTypeInt4>();

	CColumnFactory cf;

	// Exercise all three EUsedStatus values through the PcrCopy round-trip.
	const CColRef::EUsedStatus rgUsage[] = {
		CColRef::EUsed,
		CColRef::EUnused,
		CColRef::EUnknown};

	for (ULONG ul = 0; ul < GPOS_ARRAY_SIZE(rgUsage); ul++)
	{
		CWStringConst str(GPOS_WSZ_LIT("col"));

		// Create a CColRefTable with the desired EUsedStatus. PcrCopy
		// short-circuits computed colrefs, so we need the table variant
		// to exercise the lossy code path that was fixed.
		// Use large explicit ids to avoid colliding with PcrCopy's
		// internal m_aul counter on the second/third iterations.
		const ULONG ulId = 1000 + ul;
		CColRef *pcrOriginal = cf.PcrCreate(
			pmdtypeint4, default_type_modifier, rgUsage[ul],
			nullptr /*mdid_table*/, ul /*attno*/, false /*is_nullable*/,
			ulId /*id*/, CName(&str), 0 /*ulOpSource*/, false /*isDistCol*/);

		// Sanity: original carries the requested usage.
		GPOS_UNITTEST_ASSERT(rgUsage[ul] ==
							 pcrOriginal->GetUsage(false /*system_eq_used*/,
												   false /*dist_eq_used*/));

		CColRef *pcrCopy = cf.PcrCopy(pcrOriginal);

		// Contract: PcrCopy must preserve EUsedStatus. Previously this
		// failed for EUnused (copy ends up as EUnknown because the BOOL
		// collapse loses the distinction).
		GPOS_UNITTEST_ASSERT(rgUsage[ul] ==
							 pcrCopy->GetUsage(false /*system_eq_used*/,
											   false /*dist_eq_used*/));

		cf.Destroy(pcrCopy);
		cf.Destroy(pcrOriginal);
	}

	return GPOS_OK;
}


// EOF
