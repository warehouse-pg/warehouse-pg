//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CLogicalWindowTopN.h
//
//	@doc:
//		Logical operator keeping, per window partition, only the rows whose
//		rank-like window function value stays within a bound.  It is the
//		top-N pre-filter of a window's input and only ever appears
//		directly below the CLogicalSequenceProject whose output is filtered
//		(see CXformSelect2WindowTopN).
//---------------------------------------------------------------------------
#ifndef GPOPT_CLogicalWindowTopN_H
#define GPOPT_CLogicalWindowTopN_H

#include "gpos/base.h"

#include "gpopt/base/CDistributionSpec.h"
#include "gpopt/base/COrderSpec.h"
#include "gpopt/operators/CExpressionHandle.h"
#include "gpopt/operators/CLogical.h"

namespace gpopt
{
// which window function the bound applies to
enum EWindowTopNKind
{
	EwtnRowNumber = 0,
	EwtnRank,
	EwtnDenseRank,
	EwtnSentinel
};

//---------------------------------------------------------------------------
//	@class:
//		CLogicalWindowTopN
//
//	@doc:
//		Per-partition top-N by rank
//
//---------------------------------------------------------------------------
class CLogicalWindowTopN : public CLogical
{
private:
	// partition by specification of the window
	CDistributionSpec *m_pds;

	// order by specification of the window
	COrderSpec *m_pos;

	// window function the bound applies to
	EWindowTopNKind m_kind;

	// rows with a function value <= m_limit are kept
	LINT m_limit;

	// partition and order columns
	CColRefSet *m_pcrsUsed;

public:
	CLogicalWindowTopN(const CLogicalWindowTopN &) = delete;

	// ctor for pattern
	explicit CLogicalWindowTopN(CMemoryPool *mp);

	// ctor
	CLogicalWindowTopN(CMemoryPool *mp, CDistributionSpec *pds,
					   COrderSpec *pos, EWindowTopNKind kind, LINT limit);

	// dtor
	~CLogicalWindowTopN() override;

	// ident accessors
	EOperatorId
	Eopid() const override
	{
		return EopLogicalWindowTopN;
	}

	const CHAR *
	SzId() const override
	{
		return "CLogicalWindowTopN";
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

	// sensitivity to order of inputs
	BOOL
	FInputOrderSensitive() const override
	{
		return true;
	}

	// hash function
	ULONG HashValue() const override;

	// return a copy of the operator with remapped columns
	COperator *PopCopyWithRemappedColumns(CMemoryPool *mp,
										  UlongToColRefMap *colref_mapping,
										  BOOL must_exist) override;

	// print
	IOstream &OsPrint(IOstream &os) const override;

	//-------------------------------------------------------------------------------------
	// Derived Relational Properties
	//-------------------------------------------------------------------------------------

	// derive output columns
	CColRefSet *DeriveOutputColumns(CMemoryPool *mp,
									CExpressionHandle &exprhdl) override;

	// derive outer references
	CColRefSet *DeriveOuterReferences(CMemoryPool *mp,
									  CExpressionHandle &exprhdl) override;

	// derive key collections
	CKeyCollection *
	DeriveKeyCollection(CMemoryPool *,	// mp
						CExpressionHandle &exprhdl) const override
	{
		return PkcDeriveKeysPassThru(exprhdl, 0 /* ulChild */);
	}

	// derive max card
	CMaxCard
	DeriveMaxCard(CMemoryPool *,  // mp
				  CExpressionHandle &exprhdl) const override
	{
		return exprhdl.DeriveMaxCard(0);
	}

	// derive partition consumer info
	CPartInfo *
	DerivePartitionInfo(CMemoryPool *,	// mp
						CExpressionHandle &exprhdl) const override
	{
		return PpartinfoPassThruOuter(exprhdl);
	}

	// derive constraint property
	CPropConstraint *
	DerivePropertyConstraint(CMemoryPool *,	 //mp,
							 CExpressionHandle &exprhdl) const override
	{
		return PpcDeriveConstraintPassThru(exprhdl, 0 /*ulChild*/);
	}

	// derive table descriptor
	CTableDescriptorHashSet *
	DeriveTableDescriptor(CMemoryPool *,
						  CExpressionHandle &exprhdl) const override
	{
		CTableDescriptorHashSet *table_descriptor_set =
			exprhdl.DeriveTableDescriptor(0);
		table_descriptor_set->AddRef();
		return table_descriptor_set;
	}

	// compute required stats columns of the n-th child
	CColRefSet *PcrsStat(CMemoryPool *mp, CExpressionHandle &exprhdl,
						 CColRefSet *pcrsInput,
						 ULONG child_index) const override;

	//-------------------------------------------------------------------------------------
	// Transformations
	//-------------------------------------------------------------------------------------

	// candidate set of xforms
	CXformSet *PxfsCandidates(CMemoryPool *) const override;

	// stat promise
	EStatPromise
	Esp(CExpressionHandle &) const override
	{
		return CLogical::EspHigh;
	}

	// derive statistics
	IStatistics *PstatsDerive(CMemoryPool *mp, CExpressionHandle &exprhdl,
							  IStatisticsArray *stats_ctxt) const override;

	// conversion function
	static CLogicalWindowTopN *
	PopConvert(COperator *pop)
	{
		GPOS_ASSERT(nullptr != pop);
		GPOS_ASSERT(EopLogicalWindowTopN == pop->Eopid());

		return dynamic_cast<CLogicalWindowTopN *>(pop);
	}

	// name of a kind, for printing
	static const CHAR *SzKind(EWindowTopNKind kind);

};	// class CLogicalWindowTopN

}  // namespace gpopt

#endif	// !GPOPT_CLogicalWindowTopN_H

// EOF
