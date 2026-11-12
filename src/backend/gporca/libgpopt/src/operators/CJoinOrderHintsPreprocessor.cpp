//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright 2024 VMware, Inc. or its affiliates.
//
//	@filename:
//		CJoinOrderHintsPreprocessor.cpp
//
//	@doc:
//		Preprocessing routines of join order hints
//---------------------------------------------------------------------------
#include "gpopt/operators/CJoinOrderHintsPreprocessor.h"

#include <limits>

#include "gpopt/operators/CExpression.h"
#include "gpopt/operators/CLogicalInnerJoin.h"
#include "gpopt/operators/CLogicalLeftOuterJoin.h"
#include "gpopt/operators/CLogicalNAryJoin.h"
#include "gpopt/operators/CLogicalRightOuterJoin.h"
#include "gpopt/operators/CLogicalSelect.h"
#include "gpopt/operators/CPredicateUtils.h"
#include "gpopt/operators/CScalarNAryJoinPredList.h"
#include "gpopt/optimizer/COptimizerConfig.h"

using namespace gpopt;

// MAX_INDEX is a special value that indicates whether a valid predicate was
// found while applying a hint on a LOJ/ROJ.
constexpr ULONG MAX_INDEX = std::numeric_limits<ULONG>::max();


//---------------------------------------------------------------------------
//	@function:
//		GetChild
//
//	@doc:
//		Return the child expression of pexpr that has a table descriptor set
//		containing the aliases. If none exists, return nullptr.
//---------------------------------------------------------------------------
static CExpression *
GetChildMatchingAlias(CMemoryPool *mp, CExpression *pexpr,
					  StringPtrArray *aliases, ULONG *joinChildPredIndex,
					  gpos::set<ULONG> &usedChildrenIndexes)
{
	GPOS_ASSERT(COperator::EopLogicalNAryJoin == pexpr->Pop()->Eopid());

	for (ULONG ul = 0; ul < pexpr->Arity() - 1; ul++)
	{
		StringPtrArray *pexpr_aliases =
			CHintUtils::GetAliasesFromTableDescriptors(
				mp, (*pexpr)[ul]->DeriveTableDescriptor());

		bool is_contained = true;

		for (ULONG j = 0; j < aliases->Size(); j++)
		{
			if (nullptr == pexpr_aliases->Find((*aliases)[j]))
			{
				is_contained = false;
				break;
			}
		}

		pexpr_aliases->Release();
		if (is_contained)
		{
			if (CLogicalNAryJoin::PopConvert(pexpr->Pop())
					->HasOuterJoinChildren())
			{
				CLogicalNAryJoin *naryop =
					CLogicalNAryJoin::PopConvert(pexpr->Pop());

				*joinChildPredIndex = *(*naryop->GetLojChildPredIndexes())[ul];

				if (!naryop->IsInnerJoinChild(ul) &&
					CUtils::FContainsScalarIdentNullTest(
						naryop->GetOnPredicateForLOJChild(pexpr, ul)))
				{
					// LOJ order cannot be reordered if a join predicate is
					// satisified when the column of a table is NULL. This is
					// because the number of NULLs can change based on the
					// order the join is executed.
					//
					// We conservatively disregard hints if the LOJ doesn't
					// have a NULL rejecting predicate. But there are scenarios
					// where we could apply them depending on the query graph
					// structure.
					//
					// See "Outerjoin Simplification and Reordering for Query
					// Optimization" [1] Section 2.2 Join/outerjoin
					// associativity equation (8).
					//
					// [1] https://dl.acm.org/doi/10.1145/244810.244812
					return nullptr;
				}
			}
			usedChildrenIndexes.insert(ul);

			return (*pexpr)[ul];
		}
	}

	return nullptr;
}

//---------------------------------------------------------------------------
//	@function:
//		IsChildMatchingAlias
//
//	@doc:
//		Return whether a child expression of pexpr that has a table descriptor
//		set containing the aliases.
//---------------------------------------------------------------------------
static bool
IsChildMatchingAlias(CMemoryPool *mp, CExpression *pexpr,
					 StringPtrArray *aliases, ULONG *joinChildPredIndex,
					 gpos::set<ULONG> &usedChildrenIndexes)
{
	return nullptr != GetChildMatchingAlias(mp, pexpr, aliases,
											joinChildPredIndex,
											usedChildrenIndexes);
}


//---------------------------------------------------------------------------
//	@function:
//		IsAliasSubsetOrDisjoint
//
//	@doc:
//		Return whether a table descriptor set contains all or none of the
//		aliases.
//---------------------------------------------------------------------------
GPOS_ASSERTS_ONLY static bool
IsAliasSubsetOrDisjoint(CTableDescriptorHashSet *tabdescs,
						StringPtrArray *aliases)
{
	int includedCount = 0;
	int excludedCount = 0;
	CTableDescriptorHashSetIter hsiter(tabdescs);
	while (hsiter.Advance())
	{
		CTableDescriptor *tabdesc =
			const_cast<CTableDescriptor *>(hsiter.Get());
		if (nullptr == aliases->Find(tabdesc->Name().Pstr()))
		{
			excludedCount += 1;
		}
		else
		{
			includedCount += 1;
		}

		if (excludedCount > 0 && includedCount > 0)
		{
			return false;
		}
	}
	return true;
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrderHintsPreprocessor::ConstructNAryJoinChildren
//
//	@doc:
//		Return list of children that will be used to construct the new join
//		expression, with all the join order hints applied. Note that a valid
//		NAry join requires at least 2 relational children. If there aren't at
//		least 2, then the caller should take appropriate actions.
//
//		"naryJoinPexpr" is the original NAry join and "binaryJoinExpr" is the
//		processed version with hints applied. Some children from the original
//		NAry join "naryJoinPexpr" may not be included in "binaryJoinExpr"
//		because they were not used in the hints. This function adds both
//		processed and unused children in the returned expression array
//
//		Note: The hint itself is not the source of truth, it's the binary join
//		expression. Consider a future when a hint may be partially applied.
//
//		"sourceChildIndexes" receives one entry per returned child: the index
//		of the "naryJoinPexpr" child it was built from, or gpos::ulong_max for
//		"binaryJoinExpr". The caller uses it to rebuild per-child information
//		of the N-ary join, such as the LOJ predicate indexes.
//---------------------------------------------------------------------------
CExpressionArray *
CJoinOrderHintsPreprocessor::ConstructNAryJoinChildren(
	CMemoryPool *mp, CExpression *naryJoinPexpr, CExpression *binaryJoinExpr,
	gpos::set<ULONG> &usedChildrenIndexes, ULongPtrArray **sourceChildIndexes)
{
	GPOS_ASSERT(COperator::EopLogicalNAryJoin == naryJoinPexpr->Pop()->Eopid());

	// get all the alias used in the binaryJoinExpr
	StringPtrArray *usedNames = CHintUtils::GetAliasesFromTableDescriptors(
		mp, binaryJoinExpr->DeriveTableDescriptor());

	CExpressionArray *naryChildren = GPOS_NEW(mp) CExpressionArray(mp);
	*sourceChildIndexes = GPOS_NEW(mp) ULongPtrArray(mp);
	bool binaryJoinExprAdded = false;

	// For each "naryJoinPexpr" child, check if the child is accounted for in
	// "binaryJoinExpr" (i.e. contained in usedNames). If it wasn't then
	// preprocess the child directly to handle nested nary join and add the
	// result to naryChildren.
	for (ULONG ul = 0; ul < naryJoinPexpr->Arity() - 1; ul++)
	{
		if ((*naryJoinPexpr)[ul]->DeriveTableDescriptor()->Size() == 0)
		{
			continue;
		}

		GPOS_ASSERT(IsAliasSubsetOrDisjoint(
			(*naryJoinPexpr)[ul]->DeriveTableDescriptor(), usedNames));

		const CWStringConst *alias = (*naryJoinPexpr)[ul]
										 ->DeriveTableDescriptor()
										 ->First()
										 ->Name()
										 .Pstr();
		if (nullptr == usedNames->Find(alias))
		{
			// The root level of the child expression is already processed,
			// and we couldn't find an applicable hint. Next try to find an
			// applicable hint one level down, using the child as the root.
			//
			// In order to handle hints on nested nary joins we must apply the
			// join order hint processor on the children. This is because
			// CJoinOrderHintsPreprocessor::PexprPreprocess() does not
			// explicity recurse into CLogicalNAryJoin operators.
			naryChildren->Append(CJoinOrderHintsPreprocessor::PexprPreprocess(
				mp, (*naryJoinPexpr)[ul], nullptr /* joinnode */));
			(*sourceChildIndexes)->Append(GPOS_NEW(mp) ULONG(ul));
		}
		else if (!binaryJoinExprAdded && usedChildrenIndexes.count(ul) == 1)
		{
			// If this child index was used to construct "binaryJoinExpr", then
			// add the binary expression to the nary children.
			//
			// Note that only 1 hint may be applied to per nary join. A hint
			// covers multiple children, but is added once as a child of the
			// new nary operator. Hence the variable `binaryJoinExprAdded`.
			naryChildren->Append(binaryJoinExpr);
			(*sourceChildIndexes)->Append(GPOS_NEW(mp) ULONG(gpos::ulong_max));
			binaryJoinExprAdded = true;
		}
	}

	usedNames->Release();
	return naryChildren;
}


//---------------------------------------------------------------------------
//	@function:
//		GetOnPreds
//
//	@doc:
//		Builds the predicates to join the inner and outer expressions. The
//		returned predicate is a conjunction of all applicable predicates in
//		allOnPreds. When "usedPreds" is given, the predicates taken are added
//		to it (borrowed pointers, owned by allOnPreds).
//---------------------------------------------------------------------------
static CExpression *
GetOnPreds(CMemoryPool *mp, CExpression *outer, CExpression *inner,
		   CExpressionArray *allOnPreds,
		   gpos::set<CExpression *> *usedPreds = nullptr)
{
	CExpressionArray *preds = GPOS_NEW(mp) CExpressionArray(mp);

	CColRefSet *innerAndOuter = GPOS_NEW(mp) CColRefSet(mp);
	innerAndOuter->Include(outer->DeriveOutputColumns());
	innerAndOuter->Include(inner->DeriveOutputColumns());

	for (ULONG ul = 0; ul < allOnPreds->Size(); ul++)
	{
		CColRefSet *predCols = (*allOnPreds)[ul]->DeriveUsedColumns();
		GPOS_ASSERT(predCols != nullptr);

		if (innerAndOuter->ContainsAll(predCols) &&
			predCols->FIntersects(outer->DeriveOutputColumns()) &&
			predCols->FIntersects(inner->DeriveOutputColumns()))
		{
			// if both inner and outer contain cover the predicate columns then
			// add the predicate.
			(*allOnPreds)[ul]->AddRef();
			preds->Append((*allOnPreds)[ul]);
			if (nullptr != usedPreds)
			{
				usedPreds->insert((*allOnPreds)[ul]);
			}
		}
	}

	if (preds->Size() > 0)
	{
		innerAndOuter->Release();
		// create a conjunction of all the found predicates.
		return CPredicateUtils::PexprConjunction(mp, preds);
	}

	preds->Release();
	innerAndOuter->Release();

	// No explicit predicate found, use "ON true".
	return CUtils::PexprScalarConstBool(mp, /*fVal*/ true, /*is_null*/ false);
}


//---------------------------------------------------------------------------
//	@function:
//		PexprRemainingInnerPreds
//
//	@doc:
//		The conjuncts of the N-ary join's inner-join predicate (its scalar
//		child, or index GPOPT_ZERO_INNER_JOIN_PRED_INDEX of the predicate
//		list when the join has outer-join children) that no hinted join took
//		as its ON predicate, as one conjunction. nullptr when none is left;
//		constant-true conjuncts do not count.
//
//		Conjuncts are matched by expression: PdrgpexprConjuncts() hands out
//		the sub-expressions of the predicate itself, so the pointers are the
//		ones the hinted joins recorded in usedInnerConjuncts.
//---------------------------------------------------------------------------
static CExpression *
PexprRemainingInnerPreds(CMemoryPool *mp, CExpression *pexprNAryJoin,
						 gpos::set<CExpression *> &usedInnerConjuncts)
{
	CExpression *innerPreds = CLogicalNAryJoin::PopConvert(pexprNAryJoin->Pop())
								  ->GetInnerJoinPreds(pexprNAryJoin);
	CExpressionArray *conjuncts =
		CPredicateUtils::PdrgpexprConjuncts(mp, innerPreds);
	CExpressionArray *remaining = GPOS_NEW(mp) CExpressionArray(mp);
	for (ULONG ul = 0; ul < conjuncts->Size(); ul++)
	{
		CExpression *conjunct = (*conjuncts)[ul];
		if (usedInnerConjuncts.count(conjunct) == 0 &&
			!CUtils::FScalarConstTrue(conjunct))
		{
			conjunct->AddRef();
			remaining->Append(conjunct);
		}
	}
	conjuncts->Release();
	if (0 == remaining->Size())
	{
		remaining->Release();
		return nullptr;
	}
	return CPredicateUtils::PexprConjunction(mp, remaining);
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrderHintsPreprocessor::RecursiveApplyJoinOrderHintsOnNAryJoin
//
//	@doc:
//		Recursively apply a hint on a Nary join expression to bottom-up build
//		a new expression using Inner and Left/Right Outer join operators.
//
//		For example, let's say the expression tree is:
//
//		  +--CLogicalNAryJoin
//		     |--CLogicalGet "t1" ("t1"), Columns: ["a" (0), "b" (1),...
//		     |--CLogicalLimit <empty> global
//		     |  |--CLogicalNAryJoin
//		     |  |  |--CLogicalGet "t2" ("t2"), Columns: ["a" (9), "b" (10),...
//		     |  |  |--CLogicalGet "t3" ("t3"), Columns: ["a" (18), "b" (19),...
//		     |  |  ...
//		     |  ...
//		     ...
//
//		And the hint is Leading((t1 (t2 t3))).
//
//		Then the output will be:
//
//		  +--CLogicalInnerJoin
//		     |--CLogicalGet "t1" ("t1"), Columns: ["a" (0), "b" (1),...
//		     |--CLogicalLimit <empty> global
//		     |  |--CLogicalInnerJoin
//		     |  |  |--CLogicalGet "t2" ("t2"), Columns: ["a" (9), "b" (10),...
//		     |  |  |--CLogicalGet "t3" ("t3"), Columns: ["a" (18), "b" (19),...
//		     |  |  ...
//		     |  ...
//		     ...
//
//		Our first step is to separate the left and right side of the hint:
//		  left-side: "t1"
//		  right-side: "(t2 t3)"
//
//		Then, apply each hint on the relevant children of Nary join. In this
//		case, the relevant child of "t1" is CLogicalGet "t1" and the relevant
//		child of "(t2 t3)" is CLogicalLimit. Since CLogicalLimit is not a leaf
//		(i.e. has more than one relation), we recursively apply the right side
//		hint "(t2 t3)" on the CLogicalLimit expression.
//
//		After the children have been processed, then construct the appropriate
//		operator to join the children together.
//---------------------------------------------------------------------------
CExpression *
CJoinOrderHintsPreprocessor::RecursiveApplyJoinOrderHintsOnNAryJoin(
	CMemoryPool *mp, CExpression *pexpr, const CJoinHint::JoinNode *joinnode,
	gpos::set<ULONG> &usedChildrenIndexes,
	gpos::set<ULONG> &usedPredicateIndexes,
	gpos::set<CExpression *> &usedInnerConjuncts, ULONG *joinChildPredIndex)
{
	GPOS_ASSERT(COperator::EopLogicalNAryJoin == pexpr->Pop()->Eopid());

	CExpression *pexprAppliedHints = nullptr;

	StringPtrArray *hint_aliases = CHintUtils::GetAliasesFromHint(mp, joinnode);
	if (joinnode->GetName())
	{
		// Base case 1:
		//
		// hint specifies name, then return the child of CLogicalNAryJoin by
		// that name.
		StringPtrArray *aliases = GPOS_NEW(mp) StringPtrArray(mp);
		aliases->Append(
			GPOS_NEW(mp) CWStringConst(mp, joinnode->GetName()->GetBuffer()));
		pexprAppliedHints = GetChildMatchingAlias(
			mp, pexpr, aliases, joinChildPredIndex, usedChildrenIndexes);
		if (nullptr != pexprAppliedHints)
		{
			pexprAppliedHints->AddRef();
		}
		hint_aliases->Release();
		aliases->Release();
		return pexprAppliedHints;
	}

	if (IsChildMatchingAlias(mp, pexpr, hint_aliases, joinChildPredIndex,
							 usedChildrenIndexes))
	{
		// Base case 2:
		//
		// If a child covers the aliases in the jointree, then recursively
		// apply the hint to that child. For example, if child is a GROUP BY or
		// LIMIT expression, like:
		//
		// Leading(T1 T2)
		// SELECT * FROM (SELECT a FROM T1, T2 LIMIT 42) q, T3;
		//
		// Note: We already have a joinnode hint we are trying to satisfy. So,
		// explicity pass that one along so we don't try to find another
		// matching hint
		pexprAppliedHints = CJoinOrderHintsPreprocessor::PexprPreprocess(
			mp,
			GetChildMatchingAlias(mp, pexpr, hint_aliases, joinChildPredIndex,
								  usedChildrenIndexes),
			joinnode);
		hint_aliases->Release();
		return pexprAppliedHints;
	}

	// Recursive case:
	//
	// If no single child covers the hint, then apply the joinnode
	// inner/outer to the CLogicalNAryJoin. For example:
	//
	// Hint: Leading((T1 T2) (T3 T4))
	// Operator: NAryJoin [T1 T2 T3 T4]
	//
	// Then left (T1 T2) and right (T3 T4) hints need to be applied to the
	// operator, and the result joined.
	ULONG outer_join_child_pred_index = MAX_INDEX;
	ULONG inner_join_child_pred_index = MAX_INDEX;
	CExpression *outer = RecursiveApplyJoinOrderHintsOnNAryJoin(
		mp, pexpr, joinnode->GetOuter(), usedChildrenIndexes,
		usedPredicateIndexes, usedInnerConjuncts, &outer_join_child_pred_index);
	CExpression *inner = RecursiveApplyJoinOrderHintsOnNAryJoin(
		mp, pexpr, joinnode->GetInner(), usedChildrenIndexes,
		usedPredicateIndexes, usedInnerConjuncts, &inner_join_child_pred_index);

	// Hint not satisfied. This can happen, for example, if joins hint
	// splits across a GROUP BY, like:
	//
	// Leading(T1 T3)
	// SELECT * FROM (SELECT a FROM T1, T2 LIMIT 42) q, T3;
	//
	// It can also happen for invalid hints on LOJ/ROJ queries.
	if (outer == nullptr || inner == nullptr ||
		((outer_join_child_pred_index == MAX_INDEX ||
		  inner_join_child_pred_index == MAX_INDEX) &&
		 CLogicalNAryJoin::PopConvert(pexpr->Pop())->HasOuterJoinChildren()))
	{
		hint_aliases->Release();
		pexpr->AddRef();
		CRefCount::SafeRelease(outer);
		CRefCount::SafeRelease(inner);

		return pexpr;
	}

	// If Nary join contains outer join, "all_on_preds" array will only have
	// one element - a scalar list
	CExpressionArray *all_on_preds = CPredicateUtils::PdrgpexprConjuncts(
		mp, (*pexpr->PdrgPexpr())[pexpr->PdrgPexpr()->Size() - 1]);

	if (CLogicalNAryJoin::PopConvert(pexpr->Pop())->HasOuterJoinChildren())
	{
		// LOJ/ROJ have the following additional restrictions:
		//
		// 1) Join predicates are non-transitive. For example:
		//
		//    SELECT * FROM t1 LEFT JOIN t2 ON t1.a=42 LEFT JOIN t3 ON t1.a>4;
		//
		//    Predicates a=42 and a>4 cannot be pushed below their respective
		//    join conditions. Otherwise NULL values may not be output
		//    correctly.
		//
		// 2) Join kind (e.g. left, right) is affected by the order. For
		//    example:
		//
		//    SELECT * FROM t1 LEFT JOIN t2 ON t1.a=42;
		//
		//    Leading((t1 t2)) requires a left join
		//    Leading((t2 t1)) requires a right join
		//
		// outer_join_child_pred_index/inner_join_child_pred_index stores the
		// values at the index in the LOJ predicate list. This is used to
		// decide whether the hint produces a left or right join.
		if (outer_join_child_pred_index != inner_join_child_pred_index)
		{
			GPOS_ASSERT(outer_join_child_pred_index != MAX_INDEX);
			GPOS_ASSERT(inner_join_child_pred_index != MAX_INDEX);

			// Send leftmost (i.e. min) predicate index to caller so that
			// parent node can use the predicate in a join further up the
			// expression tree.
			*joinChildPredIndex = std::min(outer_join_child_pred_index,
										   inner_join_child_pred_index);

			// LEFT/RIGHT JOIN use rightmost predicate index. Mark used
			ULONG current_index = std::max(outer_join_child_pred_index,
										   inner_join_child_pred_index);
			CExpression *pred = (*(*all_on_preds)[0])[current_index];
			usedPredicateIndexes.insert(current_index);

			// Example illustratating "joinChildPredIndex" and "current_index":
			//
			// /*+
			//     Leading((t2 t3))
			//  */
			// SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.a LEFT JOIN t3 ON t2.a=t3.a;
			//
			// +--CLogicalNAryJoin [0, 1, 2]
			//    |--CLogicalGet "t1" ("t1"), Columns: ["a" (0), "b" (1), ...
			//    |--CLogicalGet "t2" ("t2"), Columns: ["a" (9), "b" (10), ...
			//    |--CLogicalGet "t3" ("t3"), Columns: ["a" (18), "b" (19), ...
			//    +--CScalarNAryJoinPredList
			//       |--CScalarConst (1)
			//       |--CScalarCmp (=)
			//       |  |--CScalarIdent "a" (0)
			//       |  +--CScalarIdent "a" (9)
			//       +--CScalarCmp (=)
			//          |--CScalarIdent "a" (9)
			//          +--CScalarIdent "a" (18)
			//
			// 1st GetChildMatchingAlias() returns t2 and joinChildPredIndex=1
			// 2nd GetChildMatchingAlias() returns t3 and joinChildPredIndex=2
			//
			// t2 LEFT JOIN t3 predicate "current_index" (i.e. 2== max(1, 2))
			// corresponds to predicate "a"(9)="a"(18) and is used as the
			// scalar child of the new join.
			//
			// "joinChildPredIndex" (i.e. 1==min(1, 2)), corresponds to
			// predicate "a"(0)="a"(9) and is *not* used directly. Instead,
			// that index is passed up the stack so that it can be used a
			// predicate in a parent join.

			CColRefSet *innerAndOuterCols = GPOS_NEW(mp) CColRefSet(mp);
			innerAndOuterCols->Include(outer->DeriveOutputColumns());
			innerAndOuterCols->Include(inner->DeriveOutputColumns());

			if (innerAndOuterCols->ContainsAll(pred->DeriveUsedColumns()))
			{
				// if every used column exists in either outer or inner
				//
				// In the case of outer join, join order hints determine the
				// placement of relation as an "absolute" left or right child,
				// and the join operator adapts accordingly.
				pred->AddRef();
				if (outer_join_child_pred_index < inner_join_child_pred_index)
				{
					pexprAppliedHints = GPOS_NEW(mp)
						CExpression(mp, GPOS_NEW(mp) CLogicalLeftOuterJoin(mp),
									outer, inner, pred);
				}
				else
				{
					GPOS_ASSERT(outer_join_child_pred_index >
								inner_join_child_pred_index);

					pexprAppliedHints = GPOS_NEW(mp)
						CExpression(mp, GPOS_NEW(mp) CLogicalRightOuterJoin(mp),
									outer, inner, pred);
				}
			}
			else
			{
				CRefCount::SafeRelease(outer);
				CRefCount::SafeRelease(inner);

				// Hint created an invalid plan - return nullptr
				//
				// This can happen if the hint joined two relations that do not
				// have a valid join predicate. LOJ/ROJ predicates are not
				// transitive. For example:
				//
				// Leading((t1 (t2 t3)))
				// SELECT * FROM t1 RIGHT JOIN t2 ON t1.a=t2.a RIGHT JOIN t3 ON t1.a=t3.a;
				//
				// t3's join predicate (ON t1.a=t3.a) references t1. So we
				// cannot join t3 with t2 until we've first joined with t1.
				GPOS_ASSERT(nullptr == pexprAppliedHints);
			}
			innerAndOuterCols->Release();
		}
		else
		{
			// Hint on an inner join in an nary join that contains LOJs. For
			// example:
			//
			// Leading(((t3 t2) t1))
			// SELECT * FROM t1 JOIN t2 ON t1.a=t2.a LEFT JOIN t3 ON t2.a=t3.a;
			GPOS_ASSERT(outer_join_child_pred_index ==
						inner_join_child_pred_index);
			*joinChildPredIndex = outer_join_child_pred_index;

			// The inner-join predicate of the N-ary join is one conjunction
			// (index GPOPT_ZERO_INNER_JOIN_PRED_INDEX of the predicate list).
			// Join the two sides with the conjuncts they cover between them,
			// as GetOnPreds() does for an N-ary join without outer joins. A
			// conjunct that also references a table not joined yet, such as
			// t1.a=t4.a for the hint (t1 t2) on
			//
			// SELECT * FROM t1 JOIN t2 ON t1.a=t2.a LEFT JOIN t3 ON t2.a=t3.a
			//               JOIN t4 ON t1.a=t4.a;
			//
			// stays in the N-ary join's inner-join predicate (see
			// PexprPreprocess) and is applied once t4 is joined.
			CExpressionArray *innerConjuncts =
				CPredicateUtils::PdrgpexprConjuncts(
					mp,
					(*(*all_on_preds)[0])[GPOPT_ZERO_INNER_JOIN_PRED_INDEX]);
			CExpression *pred = GetOnPreds(mp, outer, inner, innerConjuncts,
										   &usedInnerConjuncts);
			innerConjuncts->Release();
			pexprAppliedHints = GPOS_NEW(mp) CExpression(
				mp, GPOS_NEW(mp) CLogicalInnerJoin(mp), outer, inner, pred);
		}
	}
	else
	{
		pexprAppliedHints = GPOS_NEW(mp) CExpression(
			mp, GPOS_NEW(mp) CLogicalInnerJoin(mp), outer, inner,
			GetOnPreds(mp, outer, inner, all_on_preds, &usedInnerConjuncts));
	}
	all_on_preds->Release();


	hint_aliases->Release();
	return pexprAppliedHints;
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrderHintsPreprocessor::PexprPreprocess
//
//	@doc:
//		Search for and apply join order hints on an expression.
//
//		After we have a hint there are 3 stages to apply the hint and construct
//		a new Nary join expression:
//
//		1. Apply the hint to the leaf operators.
//		2. Apply the hint to the non-leaf operators.
//		3. Add the unused children.
//
//		Steps 1 and 2 are handled in RecursiveApplyJoinOrderHintsOnNAryJoin().
//---------------------------------------------------------------------------
CExpression *
CJoinOrderHintsPreprocessor::PexprPreprocess(
	CMemoryPool *mp, CExpression *pexpr, const CJoinHint::JoinNode *joinnode)
{
	// protect against stack overflow during recursion
	GPOS_CHECK_STACK_SIZE;
	GPOS_ASSERT(nullptr != mp);
	GPOS_ASSERT(nullptr != pexpr);

	COperator *pop = pexpr->Pop();
	CJoinHint *joinhint = nullptr;

	// Search for a join order hint for this expression.
	if (nullptr == joinnode)
	{
		CPlanHint *planhint =
			COptCtxt::PoctxtFromTLS()->GetOptimizerConfig()->GetPlanHint();

		joinhint = planhint->GetJoinHint(pexpr);
		if (joinhint)
		{
			joinnode = joinhint->GetJoinNode();
		}
	}

	// Given a hint, recursively traverse the hint and (bottom-up) construct a
	// new join expression. Any leftover children are appended to the nary
	// join.
	if (COperator::EopLogicalNAryJoin == pop->Eopid() && nullptr != joinnode)
	{
		gpos::set<ULONG> usedPredicateIndexes(mp);
		gpos::set<CExpression *> usedInnerConjuncts(mp);
		gpos::set<ULONG> usedChildrenIndexes(mp);
		ULONG joinChildPredIndex = MAX_INDEX;
		CExpression *pexprAppliedHints = RecursiveApplyJoinOrderHintsOnNAryJoin(
			mp, pexpr, joinnode, usedChildrenIndexes, usedPredicateIndexes,
			usedInnerConjuncts, &joinChildPredIndex);

		if (nullptr == pexprAppliedHints)
		{
			if (joinhint)
			{
				joinhint->SetHintStatus(IHint::HINT_STATE_NOTUSED);
			}

			pexpr->AddRef();
			return pexpr;
		}

		ULongPtrArray *sourceChildIndexes = nullptr;
		CExpressionArray *naryChildren =
			ConstructNAryJoinChildren(mp, pexpr, pexprAppliedHints,
									  usedChildrenIndexes, &sourceChildIndexes);
		if (naryChildren->Size() == 1)
		{
			// 0) Hint used all nary children making the nary operator
			//    irrelevant.
			CExpression *pexprResult = (*naryChildren)[0];
			(*naryChildren)[0]->AddRef();
			naryChildren->Release();
			sourceChildIndexes->Release();

			// The hinted joins took the inner-join conjuncts that join their
			// two sides. A conjunct no join took, such as one on the right
			// side of a LEFT JOIN that cannot be pushed below it
			// (coalesce(t3.b, 0) = 0 on t1 LEFT JOIN t3), must still filter
			// the result: keep it as a Select above the hinted tree, which
			// is where the N-ary join's own expansion applies it.
			if (pexprAppliedHints != pexpr)
			{
				CExpression *remaining =
					PexprRemainingInnerPreds(mp, pexpr, usedInnerConjuncts);
				if (nullptr != remaining)
				{
					return GPOS_NEW(mp)
						CExpression(mp, GPOS_NEW(mp) CLogicalSelect(mp),
									pexprResult, remaining);
				}
			}
			return pexprResult;
		}
		else if (CLogicalNAryJoin::PopConvert(pexpr->Pop())
					 ->HasOuterJoinChildren())
		{
			CExpression *predList = (*pexpr)[pexpr->Arity() - 1];
			CExpressionArray *naryJoinPredicates =
				GPOS_NEW(mp) CExpressionArray(mp);

			// 1) The inner-join predicate: the conjuncts the hinted joins did
			//    not take as their ON predicate. Keeping a taken conjunct here
			//    would apply it a second time above the hinted join and have
			//    its selectivity counted twice.
			CExpression *remaining =
				PexprRemainingInnerPreds(mp, pexpr, usedInnerConjuncts);
			if (nullptr != remaining)
			{
				naryJoinPredicates->Append(remaining);
			}
			else
			{
				naryJoinPredicates->Append(CUtils::PexprScalarConstBool(
					mp, /*fVal*/ true, /*is_null*/ false));
			}

			// 2) The ON predicates of the LOJ children the hint did not take
			for (ULONG ul = GPOPT_ZERO_INNER_JOIN_PRED_INDEX + 1;
				 ul < predList->Arity(); ul++)
			{
				if (usedPredicateIndexes.count(ul) == 0)
				{
					(*predList)[ul]->AddRef();
					naryJoinPredicates->Append((*predList)[ul]);
				}
			}

			// 3) One LOJ predicate index per child of the new N-ary join, in
			//    the children's order: a child kept from the original join
			//    keeps its index, the hinted join gets the index its subtree
			//    reported (0 when it is an inner-join child, otherwise the
			//    index of the ON predicate joining it as the right side of an
			//    LOJ that is still part of the N-ary join). Indexes are then
			//    shifted down past the taken ON predicates, which are no
			//    longer in the list. For example:
			//
			//    Before:
			//      [0 1 3 2]
			//         ^
			//    Let hint use index 1
			//
			//    After:
			//      [0 2 1]
			const ULongPtrArray *oldPredIndexes =
				CLogicalNAryJoin::PopConvert(pexpr->Pop())
					->GetLojChildPredIndexes();
			ULongPtrArray *lojChildPredIndexes = GPOS_NEW(mp) ULongPtrArray(mp);
			GPOS_ASSERT(sourceChildIndexes->Size() == naryChildren->Size());
			for (ULONG ul = 0; ul < naryChildren->Size(); ul++)
			{
				ULONG sourceIndex = *(*sourceChildIndexes)[ul];
				ULONG predicateIndex = (gpos::ulong_max == sourceIndex)
										   ? joinChildPredIndex
										   : *(*oldPredIndexes)[sourceIndex];
				GPOS_ASSERT(MAX_INDEX != predicateIndex);
				GPOS_ASSERT(usedPredicateIndexes.count(predicateIndex) == 0);

				ULONG adjustedPredicateIndex = predicateIndex;
				for (ULONG ul2 = 0; ul2 < predicateIndex; ul2++)
				{
					if (usedPredicateIndexes.count(ul2) != 0)
					{
						adjustedPredicateIndex -= 1;
					}
				}
				lojChildPredIndexes->Append(GPOS_NEW(mp)
												ULONG(adjustedPredicateIndex));
			}
			sourceChildIndexes->Release();
			naryChildren->Append(GPOS_NEW(mp) CExpression(
				mp, GPOS_NEW(mp) CScalarNAryJoinPredList(mp),
				naryJoinPredicates));

			return GPOS_NEW(mp) CExpression(
				mp, GPOS_NEW(mp) CLogicalNAryJoin(mp, lojChildPredIndexes),
				naryChildren);
		}
		else
		{
			sourceChildIndexes->Release();

			// The hinted joins carry the conjuncts that join their sides as
			// their own predicate, so the N-ary join keeps only the rest. A
			// conjunct kept in both places would be applied twice, and one
			// that only concerns the hinted join is a predicate over a single
			// child of the N-ary join, which the join-order algorithms other
			// than exhaustive2 refuse (CXformExpandNAryJoin) or drop. With
			// nothing left, the remaining children form a cross join.
			CExpression *remaining =
				PexprRemainingInnerPreds(mp, pexpr, usedInnerConjuncts);
			if (nullptr == remaining)
			{
				remaining = CUtils::PexprScalarConstBool(mp, /*fVal*/ true,
														 /*is_null*/ false);
			}
			naryChildren->Append(remaining);

			return GPOS_NEW(mp) CExpression(
				mp, GPOS_NEW(mp) CLogicalNAryJoin(mp), naryChildren);
		}
	}

	// If either there is no hint or this not an nary join expression, then
	// recurse into our children.
	CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
	CExpressionArray *pdrgexprChildren = pexpr->PdrgPexpr();
	for (ULONG ul = 0; ul < pexpr->Arity(); ul++)
	{
		pdrgpexpr->Append(CJoinOrderHintsPreprocessor::PexprPreprocess(
			mp, (*pdrgexprChildren)[ul], joinnode));
	}

	pop->AddRef();
	return GPOS_NEW(mp) CExpression(mp, pop, pdrgpexpr);
}
