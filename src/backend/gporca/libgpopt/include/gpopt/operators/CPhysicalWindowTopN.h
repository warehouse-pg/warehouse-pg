//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CPhysicalWindowTopN.h
//
//	@doc:
//		Physical per-partition top-N by rank (WindowTopN plan node)
//---------------------------------------------------------------------------
#ifndef GPOPT_CPhysicalWindowTopN_H
#define GPOPT_CPhysicalWindowTopN_H

#include "gpos/base.h"

#include "gpopt/base/CDistributionSpec.h"
#include "gpopt/base/COrderSpec.h"
#include "gpopt/operators/CLogicalWindowTopN.h"
#include "gpopt/operators/CPhysical.h"

namespace gpopt
{
//---------------------------------------------------------------------------
//	@class:
//		CPhysicalWindowTopN
//
//	@doc:
//		Keeps the top-N rows of each window partition of its input, using a
//		hash table of partitions and a heap per partition.  It accepts any
//		input distribution and order and produces no order; a Motion and a
//		Sort are enforced above it for the SequenceProject that follows.
//
//---------------------------------------------------------------------------
class CPhysicalWindowTopN : public CPhysical
{
private:
	// partition by specification of the window
	CDistributionSpec *m_pds;

	// order by specification of the window
	COrderSpec *m_pos;

	// window function the bound applies to
	EWindowTopNKind m_kind;

	// bound
	LINT m_limit;

	// partition and order columns
	CColRefSet *m_pcrsUsed;

public:
	CPhysicalWindowTopN(const CPhysicalWindowTopN &) = delete;

	// ctor
	CPhysicalWindowTopN(CMemoryPool *mp, CDistributionSpec *pds,
						COrderSpec *pos, EWindowTopNKind kind, LINT limit);

	// dtor
	~CPhysicalWindowTopN() override;

	// ident accessors
	EOperatorId
	Eopid() const override
	{
		return EopPhysicalWindowTopN;
	}

	const CHAR *
	SzId() const override
	{
		return "CPhysicalWindowTopN";
	}

	CDistributionSpec *
	Pds() const
	{
		return m_pds;
	}

	COrderSpec *
	Pos() const
	{
		return m_pos;
	}

	EWindowTopNKind
	Kind() const
	{
		return m_kind;
	}

	LINT
	Limit() const
	{
		return m_limit;
	}

	// match function
	BOOL Matches(COperator *pop) const override;

	// hash function
	ULONG HashValue() const override;

	// sensitivity to order of inputs
	BOOL
	FInputOrderSensitive() const override
	{
		return false;
	}

	//-------------------------------------------------------------------------------------
	// Required Plan Properties
	//-------------------------------------------------------------------------------------

	// compute required output columns of the n-th child
	CColRefSet *PcrsRequired(CMemoryPool *mp, CExpressionHandle &exprhdl,
							 CColRefSet *pcrsRequired, ULONG child_index,
							 CDrvdPropArray *pdrgpdpCtxt,
							 ULONG ulOptReq) override;

	// compute required ctes of the n-th child
	CCTEReq *PcteRequired(CMemoryPool *mp, CExpressionHandle &exprhdl,
						  CCTEReq *pcter, ULONG child_index,
						  CDrvdPropArray *pdrgpdpCtxt,
						  ULONG ulOptReq) const override;

	// compute required sort order of the n-th child
	COrderSpec *PosRequired(CMemoryPool *mp, CExpressionHandle &exprhdl,
							COrderSpec *posRequired, ULONG child_index,
							CDrvdPropArray *pdrgpdpCtxt,
							ULONG ulOptReq) const override;

	// compute required distribution of the n-th child
	CDistributionSpec *PdsRequired(CMemoryPool *mp, CExpressionHandle &exprhdl,
								   CDistributionSpec *pdsRequired,
								   ULONG child_index,
								   CDrvdPropArray *pdrgpdpCtxt,
								   ULONG ulOptReq) const override;

	// compute required rewindability of the n-th child
	CRewindabilitySpec *PrsRequired(CMemoryPool *mp, CExpressionHandle &exprhdl,
									CRewindabilitySpec *prsRequired,
									ULONG child_index,
									CDrvdPropArray *pdrgpdpCtxt,
									ULONG ulOptReq) const override;

	// check if required columns are included in output columns
	BOOL FProvidesReqdCols(CExpressionHandle &exprhdl, CColRefSet *pcrsRequired,
						   ULONG ulOptReq) const override;

	//-------------------------------------------------------------------------------------
	// Derived Plan Properties
	//-------------------------------------------------------------------------------------

	// derive sort order
	COrderSpec *PosDerive(CMemoryPool *mp,
						  CExpressionHandle &exprhdl) const override;

	// derive distribution
	CDistributionSpec *PdsDerive(CMemoryPool *mp,
								 CExpressionHandle &exprhdl) const override;

	// derive rewindability
	CRewindabilitySpec *PrsDerive(CMemoryPool *mp,
								  CExpressionHandle &exprhdl) const override;

	//-------------------------------------------------------------------------------------
	// Enforced Properties
	//-------------------------------------------------------------------------------------

	// return order property enforcing type for this operator
	CEnfdProp::EPropEnforcingType EpetOrder(
		CExpressionHandle &exprhdl, const CEnfdOrder *peo) const override;

	// return distribution property enforcing type for this operator
	CEnfdProp::EPropEnforcingType EpetDistribution(
		CExpressionHandle &exprhdl,
		const CEnfdDistribution *ped) const override;

	// return rewindability property enforcing type for this operator
	CEnfdProp::EPropEnforcingType EpetRewindability(
		CExpressionHandle &exprhdl,
		const CEnfdRewindability *per) const override;

	// return true if operator passes through stats obtained from children,
	// this is used when computing stats during costing
	BOOL
	FPassThruStats() const override
	{
		return false;
	}

	// print
	IOstream &OsPrint(IOstream &) const override;

	// conversion function
	static CPhysicalWindowTopN *
	PopConvert(COperator *pop)
	{
		GPOS_ASSERT(nullptr != pop);
		GPOS_ASSERT(EopPhysicalWindowTopN == pop->Eopid());

		return dynamic_cast<CPhysicalWindowTopN *>(pop);
	}

};	// class CPhysicalWindowTopN

}  // namespace gpopt

#endif	// !GPOPT_CPhysicalWindowTopN_H

// EOF
