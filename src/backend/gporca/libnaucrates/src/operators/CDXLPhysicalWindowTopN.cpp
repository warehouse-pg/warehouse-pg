//---------------------------------------------------------------------------
//	WarehousePG
//
//	@filename:
//		CDXLPhysicalWindowTopN.cpp
//
//	@doc:
//		Implementation of DXL per-partition top-N operators
//---------------------------------------------------------------------------

#include "naucrates/dxl/operators/CDXLPhysicalWindowTopN.h"

#include "naucrates/dxl/CDXLUtils.h"
#include "naucrates/dxl/operators/CDXLNode.h"
#include "naucrates/dxl/xml/CXMLSerializer.h"

using namespace gpos;
using namespace gpdxl;

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalWindowTopN::CDXLPhysicalWindowTopN
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CDXLPhysicalWindowTopN::CDXLPhysicalWindowTopN(CMemoryPool *mp,
											   ULongPtrArray *part_by_colid_array,
											   ULONG kind, LINT limit)
	: CDXLPhysical(mp),
	  m_part_by_colid_array(part_by_colid_array),
	  m_kind(kind),
	  m_limit(limit)
{
	GPOS_ASSERT(nullptr != m_part_by_colid_array);
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalWindowTopN::~CDXLPhysicalWindowTopN
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CDXLPhysicalWindowTopN::~CDXLPhysicalWindowTopN()
{
	m_part_by_colid_array->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalWindowTopN::GetDXLOperator
//
//	@doc:
//		Operator type
//
//---------------------------------------------------------------------------
Edxlopid
CDXLPhysicalWindowTopN::GetDXLOperator() const
{
	return EdxlopPhysicalWindowTopN;
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalWindowTopN::GetOpNameStr
//
//	@doc:
//		Operator name
//
//---------------------------------------------------------------------------
const CWStringConst *
CDXLPhysicalWindowTopN::GetOpNameStr() const
{
	return CDXLTokens::GetDXLTokenStr(EdxltokenPhysicalWindowTopN);
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalWindowTopN::SerializeToDXL
//
//	@doc:
//		Serialize operator in DXL format
//
//---------------------------------------------------------------------------
void
CDXLPhysicalWindowTopN::SerializeToDXL(CXMLSerializer *xml_serializer,
									   const CDXLNode *dxlnode) const
{
	const CWStringConst *element_name = GetOpNameStr();

	xml_serializer->OpenElement(
		CDXLTokens::GetDXLTokenStr(EdxltokenNamespacePrefix), element_name);

	CWStringDynamic *part_by_cols_str =
		CDXLUtils::Serialize(m_mp, m_part_by_colid_array);
	xml_serializer->AddAttribute(CDXLTokens::GetDXLTokenStr(EdxltokenPartKeys),
								 part_by_cols_str);
	GPOS_DELETE(part_by_cols_str);

	xml_serializer->AddAttribute(
		CDXLTokens::GetDXLTokenStr(EdxltokenWindowTopNKind), m_kind);
	xml_serializer->AddAttribute(
		CDXLTokens::GetDXLTokenStr(EdxltokenWindowTopNLimit), m_limit);

	dxlnode->SerializePropertiesToDXL(xml_serializer);
	dxlnode->SerializeChildrenToDXL(xml_serializer);

	xml_serializer->CloseElement(
		CDXLTokens::GetDXLTokenStr(EdxltokenNamespacePrefix), element_name);
}

#ifdef GPOS_DEBUG
//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalWindowTopN::AssertValid
//
//	@doc:
//		Checks whether operator node is well-structured
//
//---------------------------------------------------------------------------
void
CDXLPhysicalWindowTopN::AssertValid(const CDXLNode *dxlnode,
									BOOL validate_children) const
{
	CDXLPhysical::AssertValid(dxlnode, validate_children);

	GPOS_ASSERT(EdxlwindowtopnIndexSentinel == dxlnode->Arity());

	CDXLNode *sort_col_list_dxlnode =
		(*dxlnode)[EdxlwindowtopnIndexSortColList];
	CDXLNode *child_dxlnode = (*dxlnode)[EdxlwindowtopnIndexChild];

	GPOS_ASSERT(EdxloptypeScalar ==
				sort_col_list_dxlnode->GetOperator()->GetDXLOperatorType());
	GPOS_ASSERT(EdxloptypePhysical ==
				child_dxlnode->GetOperator()->GetDXLOperatorType());

	if (validate_children)
	{
		sort_col_list_dxlnode->GetOperator()->AssertValid(sort_col_list_dxlnode,
														  validate_children);
		child_dxlnode->GetOperator()->AssertValid(child_dxlnode,
												  validate_children);
	}
}
#endif	// GPOS_DEBUG

// EOF
