//	Greenplum Database
//	Copyright (C) 2017 VMware, Inc. or its affiliates.

#include "gpopt/base/CWindowOids.h"

using namespace gpopt;

CWindowOids::CWindowOids(OID row_number_oid, OID rank_oid, OID dense_rank_oid)
{
	m_oidRowNumber = row_number_oid;
	m_oidRank = rank_oid;
	m_oidDenseRank = dense_rank_oid;
}

OID
CWindowOids::OidDenseRank() const
{
	return m_oidDenseRank;
}

OID
CWindowOids::OidRowNumber() const
{
	return m_oidRowNumber;
}

OID
CWindowOids::OidRank() const
{
	return m_oidRank;
}

CWindowOids *
CWindowOids::GetWindowOids(CMemoryPool *mp)
{
	return GPOS_NEW(mp) CWindowOids(DUMMY_ROW_NUMBER_OID, DUMMY_WIN_RANK,
									DUMMY_WIN_DENSE_RANK);
}
