//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CPhysicalWindowTopN.cpp
//
//	@doc:
//		Implementation of the physical per-partition top-N operator
//---------------------------------------------------------------------------

#include "gpopt/operators/CPhysicalWindowTopN.h"

#include "gpos/base.h"

#include "gpopt/base/CDistributionSpecAny.h"
#include "gpopt/base/CDistributionSpecReplicated.h"
#include "gpopt/base/CDrvdPropPlan.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/operators/CExpressionHandle.h"

using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::CPhysicalWindowTopN
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CPhysicalWindowTopN::CPhysicalWindowTopN(CMemoryPool *mp,
										 CDistributionSpec *pds,
										 COrderSpec *pos, EWindowTopNKind kind,
										 LINT limit)
	: CPhysical(mp), m_pds(pds), m_pos(pos), m_kind(kind), m_limit(limit)
{
	GPOS_ASSERT(nullptr != pds);
	GPOS_ASSERT(nullptr != pos);

	m_pcrsUsed = m_pos->PcrsUsed(mp);
	CColRefSet *pcrsPart = m_pds->PcrsUsed(mp);
	m_pcrsUsed->Include(pcrsPart);
	pcrsPart->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::~CPhysicalWindowTopN
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CPhysicalWindowTopN::~CPhysicalWindowTopN()
{
	m_pds->Release();
	m_pos->Release();
	m_pcrsUsed->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::Matches
//
//	@doc:
//		Match operators
//
//---------------------------------------------------------------------------
BOOL
CPhysicalWindowTopN::Matches(COperator *pop) const
{
	if (pop->Eopid() == Eopid())
	{
		CPhysicalWindowTopN *popTopN = CPhysicalWindowTopN::PopConvert(pop);

		return popTopN->Kind() == m_kind && popTopN->Limit() == m_limit &&
			   m_pds->Matches(popTopN->Pds()) &&
			   m_pos->Matches(popTopN->Pos());
	}

	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::HashValue
//
//	@doc:
//		Hash function
//
//---------------------------------------------------------------------------
ULONG
CPhysicalWindowTopN::HashValue() const
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
//		CPhysicalWindowTopN::PcrsRequired
//
//	@doc:
//		Compute required output columns of the n-th child
//
//---------------------------------------------------------------------------
CColRefSet *
CPhysicalWindowTopN::PcrsRequired(CMemoryPool *mp, CExpressionHandle &exprhdl,
								  CColRefSet *pcrsRequired, ULONG child_index,
								  CDrvdPropArray *,	 // pdrgpdpCtxt
								  ULONG				 // ulOptReq
)
{
	GPOS_ASSERT(0 == child_index);

	CColRefSet *pcrs = GPOS_NEW(mp) CColRefSet(mp, *m_pcrsUsed);
	pcrs->Union(pcrsRequired);

	CColRefSet *pcrsChildReqd =
		PcrsChildReqd(mp, exprhdl, pcrs, child_index, gpos::ulong_max);
	pcrs->Release();

	return pcrsChildReqd;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::PosRequired
//
//	@doc:
//		Compute required sort order of the n-th child: none, the node
//		hashes its input
//
//---------------------------------------------------------------------------
COrderSpec *
CPhysicalWindowTopN::PosRequired(CMemoryPool *mp,
								 CExpressionHandle &,  // exprhdl
								 COrderSpec *,		   // posRequired
								 ULONG
#ifdef GPOS_DEBUG
									 child_index
#endif	// GPOS_DEBUG
								 ,
								 CDrvdPropArray *,	// pdrgpdpCtxt
								 ULONG				// ulOptReq
) const
{
	GPOS_ASSERT(0 == child_index);

	return GPOS_NEW(mp) COrderSpec(mp);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::PdsRequired
//
//	@doc:
//		Compute required distribution of the n-th child: any, the node
//		filters each segment's rows independently
//
//---------------------------------------------------------------------------
CDistributionSpec *
CPhysicalWindowTopN::PdsRequired(CMemoryPool *mp, CExpressionHandle &exprhdl,
								 CDistributionSpec *pdsRequired,
								 ULONG child_index,
								 CDrvdPropArray *,	// pdrgpdpCtxt
								 ULONG				// ulOptReq
) const
{
	GPOS_ASSERT(0 == child_index);

	if (exprhdl.NeedsSingletonExecution() || exprhdl.HasOuterRefs())
	{
		return PdsRequireSingleton(mp, exprhdl, pdsRequired, child_index);
	}

	return GPOS_NEW(mp) CDistributionSpecAny(this->Eopid());
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::PrsRequired
//
//	@doc:
//		Compute required rewindability of the n-th child
//
//---------------------------------------------------------------------------
CRewindabilitySpec *
CPhysicalWindowTopN::PrsRequired(CMemoryPool *mp, CExpressionHandle &exprhdl,
								 CRewindabilitySpec *prsRequired,
								 ULONG child_index,
								 CDrvdPropArray *,	// pdrgpdpCtxt
								 ULONG				// ulOptReq
) const
{
	GPOS_ASSERT(0 == child_index);

	return PrsPassThru(mp, exprhdl, prsRequired, child_index);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::PcteRequired
//
//	@doc:
//		Compute required CTE map of the n-th child
//
//---------------------------------------------------------------------------
CCTEReq *
CPhysicalWindowTopN::PcteRequired(CMemoryPool *,		//mp,
								  CExpressionHandle &,	//exprhdl,
								  CCTEReq *pcter,
								  ULONG
#ifdef GPOS_DEBUG
									  child_index
#endif
								  ,
								  CDrvdPropArray *,	 //pdrgpdpCtxt,
								  ULONG				 //ulOptReq
) const
{
	GPOS_ASSERT(0 == child_index);
	return PcterPushThru(pcter);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::FProvidesReqdCols
//
//	@doc:
//		Check if required columns are included in output columns
//
//---------------------------------------------------------------------------
BOOL
CPhysicalWindowTopN::FProvidesReqdCols(CExpressionHandle &exprhdl,
									   CColRefSet *pcrsRequired,
									   ULONG  // ulOptReq
) const
{
	return FUnaryProvidesReqdCols(exprhdl, pcrsRequired);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::PosDerive
//
//	@doc:
//		Derive sort order: the kept rows come out in hash table order
//
//---------------------------------------------------------------------------
COrderSpec *
CPhysicalWindowTopN::PosDerive(CMemoryPool *mp,
							   CExpressionHandle &	// exprhdl
) const
{
	return GPOS_NEW(mp) COrderSpec(mp);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::PdsDerive
//
//	@doc:
//		Derive distribution
//
//---------------------------------------------------------------------------
CDistributionSpec *
CPhysicalWindowTopN::PdsDerive(CMemoryPool *mp,
							   CExpressionHandle &exprhdl) const
{
	CDistributionSpec *pdsOuter = exprhdl.Pdpplan(0)->Pds();

	if (CDistributionSpec::EdtStrictReplicated == pdsOuter->Edt())
	{
		return GPOS_NEW(mp) CDistributionSpecReplicated(
			CDistributionSpec::EdtTaintedReplicated);
	}

	pdsOuter->AddRef();
	return pdsOuter;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::PrsDerive
//
//	@doc:
//		Derive rewindability
//
//---------------------------------------------------------------------------
CRewindabilitySpec *
CPhysicalWindowTopN::PrsDerive(CMemoryPool *mp,
							   CExpressionHandle &exprhdl) const
{
	return PrsDerivePassThruOuter(mp, exprhdl);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::EpetOrder
//
//	@doc:
//		Return the enforcing type for order property based on this operator:
//		the output is unordered, so any required order needs a Sort above
//
//---------------------------------------------------------------------------
CEnfdProp::EPropEnforcingType
CPhysicalWindowTopN::EpetOrder(CExpressionHandle &,	 // exprhdl
							   const CEnfdOrder *peo) const
{
	GPOS_ASSERT(nullptr != peo);

	if (peo->PosRequired()->IsEmpty())
	{
		return CEnfdProp::EpetUnnecessary;
	}

	return CEnfdProp::EpetRequired;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::EpetRewindability
//
//	@doc:
//		Return the enforcing type for rewindability property based on this
//		operator
//
//---------------------------------------------------------------------------
CEnfdProp::EPropEnforcingType
CPhysicalWindowTopN::EpetRewindability(CExpressionHandle &exprhdl,
									   const CEnfdRewindability *per) const
{
	CRewindabilitySpec *prs = CDrvdPropPlan::Pdpplan(exprhdl.Pdp())->Prs();
	if (per->FCompatible(prs))
	{
		return CEnfdProp::EpetUnnecessary;
	}

	return CEnfdProp::EpetRequired;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalWindowTopN::OsPrint
//
//	@doc:
//		Debug print
//
//---------------------------------------------------------------------------
IOstream &
CPhysicalWindowTopN::OsPrint(IOstream &os) const
{
	os << SzId() << " (" << CLogicalWindowTopN::SzKind(m_kind)
	   << " <= " << m_limit << ") ";
	m_pds->OsPrint(os);
	os << " " << (*m_pos);

	return os;
}

// EOF
