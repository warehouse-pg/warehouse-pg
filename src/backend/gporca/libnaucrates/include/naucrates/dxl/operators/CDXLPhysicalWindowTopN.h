//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CDXLPhysicalWindowTopN.h
//
//	@doc:
//		Class for representing DXL per-partition top-N operators
//---------------------------------------------------------------------------
#ifndef GPDXL_CDXLPhysicalWindowTopN_H
#define GPDXL_CDXLPhysicalWindowTopN_H

#include "gpos/base.h"

#include "naucrates/dxl/operators/CDXLPhysical.h"

namespace gpdxl
{
// indices of WindowTopN elements in the children array
enum Edxlwindowtopn
{
	EdxlwindowtopnIndexProjList = 0,
	EdxlwindowtopnIndexFilter,
	EdxlwindowtopnIndexSortColList,
	EdxlwindowtopnIndexChild,
	EdxlwindowtopnIndexSentinel
};

//---------------------------------------------------------------------------
//	@class:
//		CDXLPhysicalWindowTopN
//
//	@doc:
//		Class for representing DXL per-partition top-N operators
//
//---------------------------------------------------------------------------
class CDXLPhysicalWindowTopN : public CDXLPhysical
{
private:
	// partition by column identifiers
	ULongPtrArray *m_part_by_colid_array;

	// window function the bound applies to: 0 row_number, 1 rank,
	// 2 dense_rank
	ULONG m_kind;

	// rows with a function value <= m_limit are kept
	LINT m_limit;

public:
	CDXLPhysicalWindowTopN(const CDXLPhysicalWindowTopN &) = delete;

	// ctor
	CDXLPhysicalWindowTopN(CMemoryPool *mp, ULongPtrArray *part_by_colid_array,
						   ULONG kind, LINT limit);

	// dtor
	~CDXLPhysicalWindowTopN() override;

	// accessors
	Edxlopid GetDXLOperator() const override;
	const CWStringConst *GetOpNameStr() const override;

	const ULongPtrArray *
	GetPartByColsArray() const
	{
		return m_part_by_colid_array;
	}

	ULONG
	Kind() const
	{
		return m_kind;
	}

	LINT
	Limit() const
	{
		return m_limit;
	}

	// serialize operator in DXL format
	void SerializeToDXL(CXMLSerializer *xml_serializer,
						const CDXLNode *dxlnode) const override;

	// conversion function
	static CDXLPhysicalWindowTopN *
	Cast(CDXLOperator *dxl_op)
	{
		GPOS_ASSERT(nullptr != dxl_op);
		GPOS_ASSERT(EdxlopPhysicalWindowTopN == dxl_op->GetDXLOperator());

		return dynamic_cast<CDXLPhysicalWindowTopN *>(dxl_op);
	}

#ifdef GPOS_DEBUG
	// checks whether the operator has valid structure, i.e. number and
	// types of child nodes
	void AssertValid(const CDXLNode *, BOOL validate_children) const override;
#endif	// GPOS_DEBUG
};
}  // namespace gpdxl
#endif	// !GPDXL_CDXLPhysicalWindowTopN_H

// EOF
