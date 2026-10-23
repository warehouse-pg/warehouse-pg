//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2017 VMware, Inc. or its affiliates.
//
//	@filename:
//		CWindowOids.h
//
//	@doc:
//		System specific oids for window operations
//---------------------------------------------------------------------------
#ifndef GPOPT_CWindowOids_H
#define GPOPT_CWindowOids_H

#include "gpos/common/CRefCount.h"

#include "naucrates/dxl/gpdb_types.h"

#define DUMMY_ROW_NUMBER_OID OID(7000)
#define DUMMY_WIN_RANK OID(7001)
#define DUMMY_WIN_DENSE_RANK OID(7002)


namespace gpopt
{
//---------------------------------------------------------------------------
//	@class:
//		CWindowOids
//
//	@doc:
//		GPDB specific oids
//
//---------------------------------------------------------------------------
class CWindowOids : public CRefCount
{
private:
	// oid of window operation "row_number" function
	OID m_oidRowNumber;

	// oid of window operation "rank" function
	OID m_oidRank;

	// oid of window operation "dense_rank" function
	OID m_oidDenseRank;

public:
	CWindowOids(OID row_number_oid, OID rank_oid,
				OID dense_rank_oid = DUMMY_WIN_DENSE_RANK);

	// accessor of oid value of "dense_rank" function
	OID OidDenseRank() const;

	// accessor of oid value of "row_number" function
	OID OidRowNumber() const;

	// accessor of oid value of "rank" function
	OID OidRank() const;

	// generate default window oids
	static CWindowOids *GetWindowOids(CMemoryPool *mp);

};	// class CWindowOids
}  // namespace gpopt

#endif	// !GPOPT_CWindowOids_H

// EOF
