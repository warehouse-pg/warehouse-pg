//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CLogicalWindowTopN.cpp
//
//	@doc:
//		Implementation of the per-partition top-N operator
//---------------------------------------------------------------------------

#include "gpopt/operators/CLogicalWindowTopN.h"

#include "gpos/base.h"

#include "gpopt/base/CColRefSet.h"
#include "gpopt/base/CDistributionSpecHashed.h"
#include "gpopt/base/COptCtxt.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/operators/CExpressionHandle.h"
#include "gpopt/operators/CScalarIdent.h"
#include "naucrates/statistics/CLimitStatsProcessor.h"
#include "naucrates/statistics/CStatisticsUtils.h"

using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::CLogicalWindowTopN
//
//	@doc:
//		Ctor for pattern
//
//---------------------------------------------------------------------------
CLogicalWindowTopN::CLogicalWindowTopN(CMemoryPool *mp)
	: CLogical(mp),
	  m_pds(nullptr),
	  m_pos(nullptr),
	  m_kind(EwtnSentinel),
	  m_limit(0),
	  m_pcrsUsed(nullptr)
{
	m_fPattern = true;
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::CLogicalWindowTopN
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CLogicalWindowTopN::CLogicalWindowTopN(CMemoryPool *mp, CDistributionSpec *pds,
									   COrderSpec *pos, EWindowTopNKind kind,
									   LINT limit)
	: CLogical(mp), m_pds(pds), m_pos(pos), m_kind(kind), m_limit(limit)
{
	GPOS_ASSERT(nullptr != pds);
	GPOS_ASSERT(nullptr != pos);
	GPOS_ASSERT(EwtnSentinel > kind);

	m_pcrsUsed = m_pos->PcrsUsed(mp);
	CColRefSet *pcrsPart = m_pds->PcrsUsed(mp);
	m_pcrsUsed->Include(pcrsPart);
	pcrsPart->Release();

	m_pcrsLocalUsed->Include(m_pcrsUsed);
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::~CLogicalWindowTopN
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CLogicalWindowTopN::~CLogicalWindowTopN()
{
	CRefCount::SafeRelease(m_pds);
	CRefCount::SafeRelease(m_pos);
	CRefCount::SafeRelease(m_pcrsUsed);
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::SzKind
//
//	@doc:
//		Name of a kind
//
//---------------------------------------------------------------------------
const CHAR *
CLogicalWindowTopN::SzKind(EWindowTopNKind kind)
{
	switch (kind)
	{
		case EwtnRowNumber:
			return "row_number";
		case EwtnRank:
			return "rank";
		case EwtnDenseRank:
			return "dense_rank";
		default:
			return "?";
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::HashValue
//
//	@doc:
//		Operator specific hash function
//
//---------------------------------------------------------------------------
ULONG
CLogicalWindowTopN::HashValue() const
{
	ULONG ulKind = (ULONG) m_kind;

	return gpos::CombineHashes(
		gpos::CombineHashes(COperator::HashValue(), m_pds->HashValue()),
		gpos::CombineHashes(
			m_pos->HashValue(),
			gpos::CombineHashes(gpos::HashValue<ULONG>(&ulKind),
								gpos::HashValue<LINT>(&m_limit))));
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::Matches
//
//	@doc:
//		Match function on operator level
//
//---------------------------------------------------------------------------
BOOL
CLogicalWindowTopN::Matches(COperator *pop) const
{
	if (pop->Eopid() == Eopid())
	{
		CLogicalWindowTopN *popTopN = CLogicalWindowTopN::PopConvert(pop);

		return popTopN->Kind() == m_kind && popTopN->Limit() == m_limit &&
			   m_pds->Matches(popTopN->Pds()) &&
			   m_pos->Matches(popTopN->Pos());
	}

	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::PopCopyWithRemappedColumns
//
//	@doc:
//		Return a copy of the operator with remapped columns
//
//---------------------------------------------------------------------------
COperator *
CLogicalWindowTopN::PopCopyWithRemappedColumns(CMemoryPool *mp,
											   UlongToColRefMap *colref_mapping,
											   BOOL must_exist)
{
	CDistributionSpec *pds =
		m_pds->PdsCopyWithRemappedColumns(mp, colref_mapping, must_exist);
	COrderSpec *pos =
		m_pos->PosCopyWithRemappedColumns(mp, colref_mapping, must_exist);

	return GPOS_NEW(mp) CLogicalWindowTopN(mp, pds, pos, m_kind, m_limit);
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::DeriveOutputColumns
//
//	@doc:
//		Derive output columns
//
//---------------------------------------------------------------------------
CColRefSet *
CLogicalWindowTopN::DeriveOutputColumns(CMemoryPool *,	// mp
										CExpressionHandle &exprhdl)
{
	return PcrsDeriveOutputPassThru(exprhdl);
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::DeriveOuterReferences
//
//	@doc:
//		Derive outer references
//
//---------------------------------------------------------------------------
CColRefSet *
CLogicalWindowTopN::DeriveOuterReferences(CMemoryPool *mp,
										  CExpressionHandle &exprhdl)
{
	return CLogical::DeriveOuterReferences(mp, exprhdl, m_pcrsUsed);
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::PxfsCandidates
//
//	@doc:
//		Get candidate xforms
//
//---------------------------------------------------------------------------
CXformSet *
CLogicalWindowTopN::PxfsCandidates(CMemoryPool *mp) const
{
	CXformSet *xform_set = GPOS_NEW(mp) CXformSet(mp);

	(void) xform_set->ExchangeSet(CXform::ExfImplementWindowTopN);

	return xform_set;
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::PcrsStat
//
//	@doc:
//		Compute required stat columns of the n-th child
//
//---------------------------------------------------------------------------
CColRefSet *
CLogicalWindowTopN::PcrsStat(CMemoryPool *mp, CExpressionHandle &exprhdl,
							 CColRefSet *pcrsInput, ULONG child_index) const
{
	GPOS_ASSERT(0 == child_index);

	// the partition and sort columns are needed on top of what the parent
	// asked for; there is no scalar child, so PcrsReqdChildStats() does not
	// apply here
	CColRefSet *pcrs = GPOS_NEW(mp) CColRefSet(mp);
	pcrs->Union(pcrsInput);
	pcrs->Union(m_pcrsUsed);
	pcrs->Intersection(exprhdl.DeriveOutputColumns(child_index));

	return pcrs;
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::OsPrint
//
//	@doc:
//		Debug print
//
//---------------------------------------------------------------------------
IOstream &
CLogicalWindowTopN::OsPrint(IOstream &os) const
{
	os << SzId() << " (" << SzKind(m_kind) << " <= " << m_limit << ") ";
	m_pds->OsPrint(os);
	os << " " << (*m_pos);

	return os;
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalWindowTopN::PstatsDerive
//
//	@doc:
//		Derive statistics: each partition present on a segment keeps at most
//		the bound many rows (more only for ties), so the output is bounded by
//		bound * partitions * segments.
//
//---------------------------------------------------------------------------
IStatistics *
CLogicalWindowTopN::PstatsDerive(CMemoryPool *mp, CExpressionHandle &exprhdl,
								 IStatisticsArray *	 // not used
) const
{
	GPOS_ASSERT(Esp(exprhdl) > EspNone);
	IStatistics *child_stats = exprhdl.Pstats(0);
	COptCtxt *poctxt = COptCtxt::PoctxtFromTLS();

	CDouble dPartitions(1.0);
	if (CDistributionSpec::EdtHashed == m_pds->Edt())
	{
		CDistributionSpecHashed *pdsHashed =
			CDistributionSpecHashed::PdsConvert(m_pds);
		CExpressionArray *pdrgpexpr = pdsHashed->Pdrgpexpr();
		ULongPtrArray *pdrgpul = GPOS_NEW(mp) ULongPtrArray(mp);

		for (ULONG ul = 0; ul < pdrgpexpr->Size(); ul++)
		{
			CExpression *pexpr = (*pdrgpexpr)[ul];
			if (CUtils::FScalarIdent(pexpr))
			{
				const CColRef *colref =
					CScalarIdent::PopConvert(pexpr->Pop())->Pcr();
				pdrgpul->Append(GPOS_NEW(mp) ULONG(colref->Id()));
			}
		}
		if (0 < pdrgpul->Size())
		{
			dPartitions = CStatisticsUtils::Groups(
				mp, child_stats, poctxt->GetOptimizerConfig()->GetStatsConf(),
				pdrgpul, nullptr /*keys*/);
		}
		pdrgpul->Release();
	}

	CDouble dSegments(poctxt->GetCostModel()->UlHosts());
	CDouble dRowsMax = dPartitions * CDouble(m_limit) * dSegments;

	if (child_stats->Rows() <= dRowsMax)
	{
		child_stats->AddRef();
		return child_stats;
	}

	return CLimitStatsProcessor::CalcLimitStats(
		mp, dynamic_cast<CStatistics *>(child_stats), dRowsMax);
}

// EOF
