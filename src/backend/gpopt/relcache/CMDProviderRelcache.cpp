//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Corp.
//
//	@filename:
//		CMDProviderRelcache.cpp
//
//	@doc:
//		Implementation of a relcache-based metadata provider, which uses GPDB's
//		relcache to lookup objects given their ids.
//
//	@test:
//
//
//---------------------------------------------------------------------------

extern "C" {
#include "postgres.h"

#include "nodes/plannodes.h"
#include "optimizer/orca.h"
}
#include "gpopt/gpdbwrappers.h"
#include "gpopt/mdcache/CMDAccessor.h"
#include "gpopt/relcache/CMDProviderRelcache.h"
#include "gpopt/translate/CTranslatorRelcacheToDXL.h"
#include "naucrates/dxl/CDXLUtils.h"
#include "naucrates/exception.h"
#include "naucrates/md/CMDIdColStats.h"
#include "naucrates/md/CMDIdGPDB.h"
#include "naucrates/md/CMDIdRelStats.h"

using namespace gpos;
using namespace gpdxl;
using namespace gpmd;

CWStringBase *
CMDProviderRelcache::GetMDObjDXLStr(CMemoryPool *mp, CMDAccessor *md_accessor,
									IMDId *md_id) const
{
	// not used
	return nullptr;
}

// return the requested metadata object
IMDCacheObject *
CMDProviderRelcache::GetMDObj(CMemoryPool *mp, CMDAccessor *md_accessor,
							  IMDId *mdid, IMDCacheObject::Emdtype mdtype) const
{
	IMDCacheObject *md_obj =
		CTranslatorRelcacheToDXL::RetrieveObject(mp, md_accessor, mdid, mdtype);
	GPOS_ASSERT(nullptr != md_obj);

	return md_obj;
}

// A table an extension reads itself (orca_relation_hook) has metadata from
// the extension's hooks, whose answers can change without an invalidation:
// whether the table is the extension's, its size and its column statistics
// (which may also depend on the user). Those are looked up for each query.
BOOL
CMDProviderRelcache::Cacheable(IMDId *mdid) const
{
	IMDId *rel_mdid;
	switch (mdid->MdidType())
	{
		case IMDId::EmdidRel:
			rel_mdid = mdid;
			break;
		case IMDId::EmdidRelStats:
			rel_mdid = CMDIdRelStats::CastMdid(mdid)->GetRelMdId();
			break;
		case IMDId::EmdidColStats:
			rel_mdid = CMDIdColStats::CastMdid(mdid)->GetRelMdId();
			break;
		default:
			return true;
	}
	return ORCA_RELATION_NONE ==
		   gpdb::ExtensionRelation(CMDIdGPDB::CastMdid(rel_mdid)->Oid());
}

// EOF
