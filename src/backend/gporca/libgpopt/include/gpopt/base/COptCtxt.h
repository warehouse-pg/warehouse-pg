//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2009 Greenplum, Inc.
//
//	@filename:
//		COptCtxt.h
//
//	@doc:
//		Optimizer context object; contains all global objects pertaining to
//		one optimization
//---------------------------------------------------------------------------
#ifndef GPOPT_COptCtxt_H
#define GPOPT_COptCtxt_H

#include "gpos/base.h"
#include "gpos/task/CTaskLocalStorageObject.h"

#include "gpopt/base/CCTEInfo.h"
#include "gpopt/base/CColumnFactory.h"
#include "gpopt/base/CConstraint.h"
#include "gpopt/base/IComparator.h"
#include "gpopt/base/SPartSelectorInfo.h"
#include "gpopt/mdcache/CMDAccessor.h"
#include "gpos/utils.h"

namespace gpopt
{
using namespace gpos;

// hash maps ULONG -> array of ULONGs
using UlongToBitSetMap =
	CHashMap<ULONG, CBitSet, gpos::HashValue<ULONG>, gpos::Equals<ULONG>,
			 CleanupDelete<ULONG>, CleanupRelease<CBitSet>>;

// forward declarations
class CColRefSet;
class CConstraintInterval;
class CExpression;
class COptimizerConfig;
class ICostModel;
class IConstExprEvaluator;

//---------------------------------------------------------------------------
//	@struct:
//		SArrayCnstrCacheKey
//
//	@doc:
//		Key for caching the CConstraintInterval derived from a
//		"col <op> ANY/ALL (const array)" expression: the CONTENT of the array
//		expression, the comparison operator, ANY/ALL, the column and
//		infer_nulls_as.
//
//		The key is content based, not pointer based:
//		  - an equal array that is translated again (for example the CHECK
//		    constraint path builds a new expression on every derivation) finds
//		    the entry built earlier;
//		  - an entry can never be returned for a different array, even if an
//		    operator is shared between expressions.
//
//		The key holds a reference on the array expression and on the operator
//		mdid for its whole lifetime, so their memory cannot be recycled while
//		an entry exists. Copying is deleted because a copy would release
//		those references twice.
//
//---------------------------------------------------------------------------
struct SArrayCnstrCacheKey
{
	CExpression *m_pexprArray;
	IMDId *m_mdid_op;
	ULONG m_earrcmpt;  // CScalarArrayCmp::EArrCmpType
	const CColRef *m_pcr;
	BOOL m_infer_nulls_as;

	SArrayCnstrCacheKey(CExpression *pexprArray, IMDId *mdid_op,
						ULONG earrcmpt, const CColRef *pcr,
						BOOL infer_nulls_as);

	SArrayCnstrCacheKey(const SArrayCnstrCacheKey &) = delete;
	SArrayCnstrCacheKey &operator=(const SArrayCnstrCacheKey &) = delete;

	~SArrayCnstrCacheKey();

	static ULONG HashValue(const SArrayCnstrCacheKey *pkey);

	static BOOL Equals(const SArrayCnstrCacheKey *pkey1,
					   const SArrayCnstrCacheKey *pkey2);
};

// hash map: SArrayCnstrCacheKey -> CConstraint*
// Values are always CConstraintInterval* but stored as base class to avoid
// including CConstraintInterval.h in this widely-included header.
using ArrayCnstrCacheMap =
	CHashMap<SArrayCnstrCacheKey, CConstraint,
			 SArrayCnstrCacheKey::HashValue, SArrayCnstrCacheKey::Equals,
			 CleanupDelete<SArrayCnstrCacheKey>,
			 CleanupRelease<CConstraint>>;

//---------------------------------------------------------------------------
//	@class:
//		COptCtxt
//
//	@doc:
//		"Optimizer Context" is a container of global objects (mostly
//		singletons) that are needed by the optimizer.
//
//		A COptCtxt object is instantiated in COptimizer::PdxlnOptimize() via
//		COptCtxt::PoctxtCreate() and stored as a task local object. The global
//		information contained in it can be accessed by calling
//		COptCtxt::PoctxtFromTLS(), instead of passing a pointer to it all
//		around. For example to get the global CMDAccessor:
//			CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();
//
//---------------------------------------------------------------------------
class COptCtxt : public CTaskLocalStorageObject
{
private:
	// shared memory pool
	CMemoryPool *m_mp;

	// column factory
	CColumnFactory *m_pcf;

	// metadata accessor;
	CMDAccessor *m_pmda;

	// cost model
	ICostModel *m_cost_model;

	// constant expression evaluator
	IConstExprEvaluator *m_pceeval;

	// comparator between IDatum instances
	IComparator *m_pcomp;

	// atomic counter for generating part index ids
	ULONG m_auPartId;

	// global CTE information
	CCTEInfo *m_pcteinfo;

	// system columns required in query output
	CColRefArray *m_pdrgpcrSystemCols;

	// optimizer configurations
	COptimizerConfig *m_optimizer_config;

	// whether or not we are optimizing a DML query
	BOOL m_fDMLQuery;

	// value for the first valid part id
	static ULONG m_ulFirstValidPartId;

	// if there are coordinator only tables in the query
	BOOL m_has_coordinator_only_tables;

	// does the query contain any volatile functions or
	// functions that read/modify SQL data
	BOOL m_has_volatile_func{false};

	// does the query have replicated tables
	BOOL m_has_replicated_tables;

	// does this plan have a direct dispatchable filter
	CExpressionArray *m_direct_dispatchable_filters;

	// mappings of dynamic scan -> partition indexes (after static elimination)
	// this is mainetained here to avoid dependencies on optimization order
	// between dynamic scans/partition selectors and remove the assumption
	// of one being optimized before the other. Instead, we populate the
	// partitions during optimization of the dynamic scans, and populate
	// the partitions for the corresponding partition selector in
	// ExprToDXL. We could possibly do this in DXLToPlstmt, but we would be
	// making an assumption about the order the scan vs partition selector
	// is translated, and would also need information from the append's
	// child dxl nodes.
	UlongToBitSetMap *m_scanid_to_part_map;

	// unique id per partition selector in the memo
	ULONG m_selector_id_counter;

	// detailed info (filter expr, stats etc) per partition selector
	// (required by CDynamicPhysicalScan for recomputing statistics for DPE)
	SPartSelectorInfo *m_part_selector_info;

	// cache for CConstraintInterval derived from ScalarArrayCmp expressions,
	// avoids repeated O(N log N) sort+dedup. Allocated on first insert, so a
	// query without large IN lists pays nothing.
	ArrayCnstrCacheMap *m_phmArrayCnstrCache;

	// number of times an IN/NOT IN constraint derivation was skipped because
	// the array exceeded optimizer_array_interval_threshold. A skipped
	// derivation means "no information" (fine when the predicate is
	// AND-ed), but complementing a result that was derived without it would
	// be unsound, so NOT checks whether this changed while deriving its child.
	ULONG m_ulArrayCnstrBailouts;

public:
	COptCtxt(COptCtxt &) = delete;

	// ctor
	COptCtxt(CMemoryPool *mp, CColumnFactory *col_factory,
			 CMDAccessor *md_accessor, IConstExprEvaluator *pceeval,
			 COptimizerConfig *optimizer_config);

	// dtor
	~COptCtxt() override;

	// memory pool accessor
	CMemoryPool *
	Pmp() const
	{
		return m_mp;
	}

	// optimizer configurations
	COptimizerConfig *
	GetOptimizerConfig() const
	{
		return m_optimizer_config;
	}

	// are we optimizing a DML query
	BOOL
	FDMLQuery() const
	{
		return m_fDMLQuery;
	}

	// set the DML flag
	void
	MarkDMLQuery(BOOL fDMLQuery)
	{
		m_fDMLQuery = fDMLQuery;
	}

	void
	SetHasCoordinatorOnlyTables()
	{
		m_has_coordinator_only_tables = true;
	}

	void
	SetHasVolatileFunc()
	{
		m_has_volatile_func = true;
	}

	void
	SetHasReplicatedTables()
	{
		m_has_replicated_tables = true;
	}

	void
	AddDirectDispatchableFilterCandidate(CExpression *filter_expression)
	{
		filter_expression->AddRef();
		m_direct_dispatchable_filters->Append(filter_expression);
	}

	BOOL
	HasCoordinatorOnlyTables() const
	{
		return m_has_coordinator_only_tables;
	}

	BOOL
	HasVolatileFunc() const
	{
		return m_has_volatile_func;
	}

	BOOL
	HasReplicatedTables() const
	{
		return m_has_replicated_tables;
	}

	CExpressionArray *
	GetDirectDispatchableFilters() const
	{
		return m_direct_dispatchable_filters;
	}

	BOOL
	OptimizeDMLQueryWithSingletonSegment() const
	{
		// A DML statement can be optimized by enforcing a gather motion on segment instead of coordinator,
		// whenever a singleton execution is needed.
		// This optmization can not be applied if the query contains any of the following:
		// (1). coordinator-only tables
		// (2). a volatile function
		return !GPOS_FTRACE(EopttraceDisableNonCoordinatorGatherForDML) &&
			   FDMLQuery() && !HasCoordinatorOnlyTables() && !HasVolatileFunc();
	}

	// column factory accessor
	CColumnFactory *
	Pcf() const
	{
		return m_pcf;
	}

	// metadata accessor
	CMDAccessor *
	Pmda() const
	{
		return m_pmda;
	}

	// cost model accessor
	ICostModel *
	GetCostModel() const
	{
		return m_cost_model;
	}

	// constant expression evaluator
	IConstExprEvaluator *
	Pceeval()
	{
		return m_pceeval;
	}

	// comparator
	const IComparator *
	Pcomp()
	{
		return m_pcomp;
	}

	// cte info
	CCTEInfo *
	Pcteinfo()
	{
		return m_pcteinfo;
	}

	// return a new part index id
	ULONG
	UlPartIndexNextVal()
	{
		return m_auPartId++;
	}

	ULONG
	NextPartSelectorId()
	{
		return m_selector_id_counter++;
	}

	// required system columns
	CColRefArray *
	PdrgpcrSystemCols() const
	{
		return m_pdrgpcrSystemCols;
	}

	void AddPartForScanId(ULONG scanid, ULONG index);

	CBitSet *
	GetPartitionsForScanId(ULONG scanid)
	{
		return m_scanid_to_part_map->Find(&scanid);
	}

	BOOL AddPartSelectorInfo(ULONG selector_id, SPartSelectorInfoEntry *entry);

	const SPartSelectorInfoEntry *GetPartSelectorInfo(ULONG selector_id) const;

	// lookup the cached CConstraintInterval for "pcr <op> ANY/ALL (pexprArray)"
	// returns AddRef'd interval if found, nullptr otherwise
	CConstraintInterval *PciLookupArrayCnstrCache(CExpression *pexprArray,
												  IMDId *mdid_op,
												  ULONG earrcmpt,
												  const CColRef *pcr,
												  BOOL infer_nulls_as);

	// cache pci for "pcr <op> ANY/ALL (pexprArray)"; the cache takes its own
	// reference. Only intervals allocated from the optimization memory pool
	// are cached (the cache lives until ~COptCtxt), so mp must be that pool,
	// otherwise the interval is not cached.
	void InsertArrayCnstrCache(CMemoryPool *mp, CExpression *pexprArray,
							   IMDId *mdid_op, ULONG earrcmpt,
							   const CColRef *pcr, BOOL infer_nulls_as,
							   CConstraintInterval *pci);

	// record that an IN/NOT IN derivation was skipped (threshold exceeded)
	void
	NoteArrayCnstrBailout()
	{
		m_ulArrayCnstrBailouts++;
	}

	ULONG
	UlArrayCnstrBailouts() const
	{
		return m_ulArrayCnstrBailouts;
	}

	// set required system columns
	void
	SetReqdSystemCols(CColRefArray *pdrgpcrSystemCols)
	{
		GPOS_ASSERT(nullptr != pdrgpcrSystemCols);

		CRefCount::SafeRelease(m_pdrgpcrSystemCols);
		m_pdrgpcrSystemCols = pdrgpcrSystemCols;
	}

	// factory method
	static COptCtxt *PoctxtCreate(CMemoryPool *mp, CMDAccessor *md_accessor,
								  IConstExprEvaluator *pceeval,
								  COptimizerConfig *optimizer_config);

	// shorthand to retrieve opt context from TLS
	inline static COptCtxt *
	PoctxtFromTLS()
	{
		return reinterpret_cast<COptCtxt *>(
			ITask::Self()->GetTls().Get(CTaskLocalStorage::EtlsidxOptCtxt));
	}

	// return true if all enforcers are enabled
	static BOOL FAllEnforcersEnabled();

};	// class COptCtxt
}  // namespace gpopt


#endif	// !GPOPT_COptCtxt_H

// EOF
