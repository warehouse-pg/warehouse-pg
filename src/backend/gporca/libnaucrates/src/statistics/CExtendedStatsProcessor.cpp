//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright 2023 VMware, Inc. or its affiliates.
//
//	@filename:
//		CExtendedStatsProcessor.cpp
//
//	@doc:
//		Statistics helper routines for processing extended statistics.
//
//		Many functions in this file are mirrored versions of functions in
//		dependencies.c and selfuncs.c. Ideally they should stay in sync.
//		Unfortunately, the duplication is necessary due ORCA's DXL abstraction
//		that by design it to be independent of backend core. In other words, we
//		do not necessarily have access to backend core functions. Hence the
//		need to mirror them here.
//---------------------------------------------------------------------------

#include "naucrates/statistics/CExtendedStatsProcessor.h"

#include "gpos/common/CBitSet.h"

#include "naucrates/md/CMDExtStatsInfo.h"
#include "naucrates/statistics/CFilterStatsProcessor.h"
#include "naucrates/statistics/CStatisticsUtils.h"
#include "naucrates/statistics/CStatsPredPoint.h"


#define STATS_MAX_DIMENSIONS 8 /* max number of attributes */

#define InvalidOid 0


using namespace gpopt;

/*
 * A functional dependency states that equal values of the determining
 * columns imply an equal value of the implied column, so only an equality
 * comparison against a non-null constant can be matched to one; a range
 * comparison on a covered column says nothing about the implied column. Every
 * constant comparison is an EsptPoint predicate, and IS NULL is represented
 * as an equality with a null constant, so both the comparison type and the
 * constant have to be checked. Mirrors the backend's
 * dependency_is_compatible_clause(), which accepts equality operators only.
 */
static BOOL
IsDependencyCapablePredicate(CStatsPred *child_pred)
{
	if (CStatsPred::EsptPoint != child_pred->GetPredStatsType())
	{
		return false;
	}

	CStatsPredPoint *point_pred = CStatsPredPoint::ConvertPredStats(child_pred);

	return CStatsPred::EstatscmptEq == point_pred->GetCmpType() &&
		   !point_pred->GetPredPoint()->GetDatum()->IsNull();
}

/*
 * A colid -> attno mapping entry is usable for extended-statistics estimation
 * only if it exists and refers to a user column. Unlike the backend, ORCA has
 * to translate its column ids back to attnos and the mapping is not
 * guaranteed to cover every column. System columns (attno <= 0) are never
 * covered by extended statistics and cannot be represented in a CBitSet; the
 * backend rejects them too (dependency_is_compatible_clause()).
 */
static BOOL
FUsableAttno(const INT *attnum)
{
	return nullptr != attnum && 0 < *attnum;
}

/*
 * choose_best_statistics
 *		Look for and return statistics with the specified 'requiredkind' which
 *		have keys that match at least two of the given attnums.  Return NULL if
 *		there's no match.
 *
 * The current selection criteria is very simple - we choose the statistics
 * object referencing the most of the requested attributes, breaking ties
 * in favor of objects with fewer keys overall.
 *
 * XXX If multiple statistics objects tie on both criteria, then which object
 * is chosen depends on the order that they appear in the stats list. Perhaps
 * further tiebreakers are needed.
 *
 * NB: This function is modified version of choose_best_statistics() in
 *     dependencies.c.
 */
CMDExtStatsInfo *
choose_best_statistics(CMemoryPool *mp,
					   CMDExtStatsInfoArray *md_statsinfo_array,
					   CBitSet *attnums,
					   CMDExtStatsInfo::Estattype requiredkind)
{
	CMDExtStatsInfo *best_match = nullptr;
	int best_num_matched = 2;						  /* goal #1: maximize */
	int best_match_keys = (STATS_MAX_DIMENSIONS + 1); /* goal #2: minimize */

	for (ULONG i = 0; i < md_statsinfo_array->Size(); i++)
	{
		CMDExtStatsInfo *info = (*md_statsinfo_array)[i];
		int num_matched;
		int numkeys;
		CBitSet *matched;

		/* skip statistics that are not of the correct type */
		if (info->GetStatKind() != requiredkind)
		{
			continue;
		}

		/* determine how many attributes of these stats can be matched to */
		matched = GPOS_NEW(mp) CBitSet(mp, *attnums);
		matched->Intersection(info->GetStatKeys());
		num_matched = matched->Size();
		matched->Release();

		/*
		 * save the actual number of keys in the stats so that we can choose
		 * the narrowest stats with the most matching keys.
		 */
		numkeys = info->GetStatKeys()->Size();

		/*
		 * Use this object when it increases the number of matched clauses or
		 * when it matches the same number of attributes but these stats have
		 * fewer keys than any previous match.
		 */
		if (num_matched > best_num_matched ||
			(num_matched == best_num_matched && numkeys < best_match_keys))
		{
			best_match = info;
			best_num_matched = num_matched;
			best_match_keys = numkeys;
		}
	}

	return best_match;
}

/*
 * dependency_implies_attribute
 *		check that the attnum matches is implied by the functional dependency
 *
 * NB: This function is modified version of dependency_implies_attribute() in
 *     dependencies.c.
 */
static bool
dependency_implies_attribute(CMDDependency *dependency, INT attnum)
{
	if (attnum == dependency->GetToAttno())
	{
		return true;
	}

	return false;
}

/*
 * dependency_is_fully_matched
 *		checks that a functional dependency is fully matched given clauses on
 *		attributes (assuming the clauses are suitable equality clauses)
 *
 * NB: This function is modified version of dependency_is_fully_matched() in
 *     dependencies.c.
 */
static bool
dependency_is_fully_matched(CMDDependency *dependency, CBitSet *attnums)
{
	/*
	 * Check that the dependency actually is fully covered by clauses. We have
	 * to translate all attribute numbers, as those are referenced
	 */
	for (ULONG j = 0; j < dependency->GetNAttributes() - 1; j++)
	{
		int attnum = *(*dependency->GetFromAttno())[j];

		if (!attnums->Get(attnum))
		{
			return false;
		}
	}

	return attnums->Get(dependency->GetToAttno());
}

/*
 * find_strongest_dependency
 *		find the strongest dependency on the attributes
 *
 * When applying functional dependencies, we start with the strongest
 * dependencies. That is, we select the dependency that:
 *
 * (a) has all attributes covered by equality clauses
 *
 * (b) has the most attributes
 *
 * (c) has the highest degree of validity
 *
 * This guarantees that we eliminate the most redundant conditions first
 * (see the comment in dependencies_clauselist_selectivity).
 *
 * NB: This function is modified version of find_strongest_dependency() in
 *     dependencies.c.
 */
static CMDDependency *
find_strongest_dependency(CMDDependencyArray *dependencies, CBitSet *attnums)
{
	ULONG i;
	CMDDependency *strongest = nullptr;

	/* number of attnums in clauses */
	ULONG nattnums = attnums->Size();

	/*
	 * Iterate over the MVDependency items and find the strongest one from the
	 * fully-matched dependencies. We do the cheap checks first, before
	 * matching it against the attnums.
	 */
	for (i = 0; i < dependencies->Size(); i++)
	{
		CMDDependency *dependency = (*dependencies)[i];

		/*
		 * Skip dependencies referencing more attributes than available
		 * clauses, as those can't be fully matched.
		 */
		if (dependency->GetNAttributes() > nattnums)
		{
			continue;
		}

		if (strongest)
		{
			/* skip dependencies on fewer attributes than the strongest. */
			if (dependency->GetNAttributes() < strongest->GetNAttributes())
			{
				continue;
			}

			/* also skip weaker dependencies when attribute count matches */
			if (strongest->GetNAttributes() == dependency->GetNAttributes() &&
				strongest->GetDegree() > dependency->GetDegree())
			{
				continue;
			}
		}

		/*
		 * this dependency is stronger, but we must still check that it's
		 * fully matched to these attnums. We perform this check last as it's
		 * slightly more expensive than the previous checks.
		 */
		if (dependency_is_fully_matched(dependency, attnums))
		{
			strongest = dependency; /* save new best match */
		}
	}

	return strongest;
}

//---------------------------------------------------------------------------
//	@function:
//		CExtendedStatsProcessor::ApplyCorrelatedStatsToScaleFactorFilterCalculation
//
//	@doc:
//		ORCA counterpart of dependencies_clauselist_selectivity() in
//		dependencies.c. Picks the functional-dependency statistics object that
//		covers the most equality conjuncts and, for each applicable dependency
//		(a,b => c), estimates the conjunct on the implied column c against its
//		histogram and folds it into the combined estimate as
//
//		    P(a,b,c) = P(a,b) * (f + (1-f) * P(c))
//
//		where f is the dependency's degree of validity: in the fraction f of
//		the rows where the dependency holds, c is fully determined by a,b and
//		the conjunct on c filters nothing further; in the remaining rows c is
//		treated as independent. The conjunct on c is marked as estimated so
//		the per-column path does not count it again, and c's histogram in
//		result_histograms is replaced by the filtered one. The conjuncts on
//		the determining columns are left to the per-column path.
//
//		Returns the resulting scale factor, i.e. the reciprocal of the product
//		of the bracketed terms (>= 1.0; exactly 1.0 when no dependency
//		applies). The caller multiplies it into the filter's scale factor
//		outside the multi-column damping: the bracket already encodes the
//		measured correlation between the covered columns, which is what the
//		damping approximates for columns without such statistics.
//
//---------------------------------------------------------------------------
CDouble
CExtendedStatsProcessor::ApplyCorrelatedStatsToScaleFactorFilterCalculation(
	CStatsPredConj *conjunctive_pred_stats, const IMDExtStatsInfo *md_statsinfo,
	UlongToIntMap *colid_to_attno_mapping, CMemoryPool *mp,
	UlongToHistogramMap *result_histograms)
{
	if (!md_statsinfo || md_statsinfo->GetExtStatInfoArray()->Size() == 0)
	{
		return CDouble(1.0);
	}

	DOUBLE dependency_scale_factor = 1.0;
	CMDExtStatsInfo *stat;
	CMDDependencyArray *dependencies;

	CBitSet *clauses_attnums = GPOS_NEW(mp) CBitSet(mp);

	/* attnums whose clause has been estimated through a dependency */
	CBitSet *estimated_attnums = GPOS_NEW(mp) CBitSet(mp);

	/*
	 * Pre-process the clauses list to extract the attnums seen in each item.
	 * We need to determine if there's any clauses which will be useful for
	 * dependency selectivity estimations. Along the way we'll record all of
	 * the attnums for each clause in a list which we'll reference later so we
	 * don't need to repeat the same work again. We'll also keep track of all
	 * attnums seen.
	 *
	 * We also skip clauses that we already estimated using different types of
	 * statistics (we treat them as incompatible).
	 */
	for (ULONG ul = 0; ul < conjunctive_pred_stats->GetNumPreds(); ul++)
	{
		CStatsPred *child_pred = conjunctive_pred_stats->GetPredStats(ul);
		if (!child_pred->IsAlreadyUsedInScaleFactorEstimation() &&
			IsDependencyCapablePredicate(child_pred))
		{
			ULONG colid = child_pred->GetColId();
			INT *attnum = colid_to_attno_mapping->Find(&colid);
			if (!FUsableAttno(attnum))
			{
				/*
				 * Skip such clauses; they fall back to the independence
				 * assumption.
				 */
				continue;
			}
			clauses_attnums->ExchangeSet(*attnum);
		}
	}

	/*
	 * If there's not at least two distinct attnums then reject the whole list
	 * of clauses.
	 */
	if (clauses_attnums->Size() < 2)
	{
		clauses_attnums->Release();
		estimated_attnums->Release();
		return CDouble(1.0);
	}

	/* find the best suited statistics object for these attnums */
	stat = choose_best_statistics(mp, md_statsinfo->GetExtStatInfoArray(),
								  clauses_attnums,
								  CMDExtStatsInfo::EstatDependencies);

	if (!stat)
	{
		clauses_attnums->Release();
		estimated_attnums->Release();
		return CDouble(1.0);
	}

	const COptCtxt *poctxt = COptCtxt::PoctxtFromTLS();
	CMDAccessor *md_accessor = poctxt->Pmda();

	CMDIdGPDB *pmdid =
		GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidExtStats, stat->GetStatOid());
	const IMDExtStats *extstats = md_accessor->RetrieveExtStats(pmdid);
	pmdid->Release();

	/* load the dependency items stored in the statistics object */
	dependencies = extstats->GetDependencies();

	/*
	 * Apply the dependencies recursively, starting with the widest/strongest
	 * ones, and proceeding to the smaller/weaker ones. At the end of each
	 * round we factor in the selectivity of clauses on the implied attribute,
	 * and remove the clauses from the list.
	 */
	while (true)
	{
		/* selectivity of the conjunct on the implied column, P(c) above */
		DOUBLE implied_sel = 1.0;
		CMDDependency *dependency;

		/* the widest/strongest dependency, fully matched by clauses */
		dependency = find_strongest_dependency(dependencies, clauses_attnums);

		/* if no suitable dependency was found, we're done */
		if (!dependency)
		{
			break;
		}

		/*
		 * We found an applicable dependency, so find all the clauses on the
		 * implied attribute - with dependency (a,b => c) we look for clauses
		 * on 'c'.
		 */
		const ULONG filters = conjunctive_pred_stats->GetNumPreds();
		for (ULONG ul = 0; ul < filters; ul++)
		{
			CStatsPred *child_pred = conjunctive_pred_stats->GetPredStats(ul);

			/*
			 * Skip incompatible clauses, and ones we've already estimated on.
			 */
			if (child_pred->IsAlreadyUsedInScaleFactorEstimation() ||
				!IsDependencyCapablePredicate(child_pred))
			{
				continue;
			}

			ULONG colid = child_pred->GetColId();
			INT *attnum = colid_to_attno_mapping->Find(&colid);
			if (!FUsableAttno(attnum))
			{
				/* no usable attno for this clause; it was never collected
				 * into clauses_attnums by the pre-processing loop above */
				continue;
			}

			if (!dependency_implies_attribute(dependency, *attnum))
			{
				continue;
			}

			if (!clauses_attnums->Get(*attnum))
			{
				/*
				 * A further equality clause on the implied attribute. If the
				 * attribute was estimated through the dependency, the first
				 * clause has already fixed its value and selectivity: with the
				 * same constant this one filters nothing more, with a
				 * different constant no row matches at all, so it must not be
				 * counted again by the per-column path. If the dependency was
				 * given up for the attribute instead, the clause stays with
				 * the per-column path like the first one.
				 */
				if (estimated_attnums->Get(*attnum))
				{
					child_pred->SetEstimated();
				}
				continue;
			}

			const CHistogram *histogram = result_histograms->Find(&colid);
			if (nullptr == histogram)
			{
				/*
				 * No histogram to estimate the implied clause with; give up
				 * on the dependency for this attribute. The attnum bit must
				 * still be cleared: the outer loop terminates only once no
				 * dependency is fully matched by the remaining attnums, so
				 * leaving the bit set would make find_strongest_dependency()
				 * return the same dependency forever. The clause stays
				 * unestimated here and is later given the default selectivity
				 * by the regular per-column path (see
				 * MakeHistHashMapConjFilter()).
				 */
				clauses_attnums->ExchangeClear(*attnum);
				continue;
			}

			/*
			 * Estimate the implied clause on its own, exactly as the
			 * per-column path would: filter the attribute's histogram by the
			 * constant and normalize it. The normalization returns the
			 * reciprocal of the clause's selectivity, and the filtered
			 * histogram replaces the unfiltered one so that downstream
			 * operators see the attribute's post-filter distribution.
			 */
			CStatsPredPoint *point_pred =
				CStatsPredPoint::ConvertPredStats(child_pred);
			CDouble clause_scale_factor(1.0);
			CHistogram *filtered_histogram =
				histogram->MakeHistogramFilterNormalize(
					point_pred->GetCmpType(), point_pred->GetPredPoint(),
					&clause_scale_factor);
			implied_sel = 1.0 / clause_scale_factor.Get();

			/* replacing the entry releases 'histogram'; do not use it below */
			CStatisticsUtils::AddHistogram(mp, colid, filtered_histogram,
										   result_histograms,
										   true /* replace_old */);
			GPOS_DELETE(filtered_histogram);

			/* mark this one as done, so we don't touch it again. */
			child_pred->SetEstimated();
			estimated_attnums->ExchangeSet(*attnum);

			/*
			 * Mark that we've got and used the dependency on this clause.
			 * We'll want to ignore this when looking for the next
			 * strongest dependency above.
			 */
			clauses_attnums->ExchangeClear(*attnum);
		}

		/*
		 * Fold the implied clause into the estimate:
		 *
		 * P(a,b) = P(a) * (f + (1-f) * P(b))
		 *
		 * where 'f' is the degree of validity of the dependency. In
		 * scale-factor space that is a division by the bracket: with f = 1 the
		 * implied clause is fully redundant and the factor is 1, with f = 0 it
		 * degenerates to the independence assumption. The bracket is a
		 * probability; clamp it so that rounding in f + (1-f) cannot push it
		 * past 1 and yield a scale factor below 1.
		 */
		DOUBLE degree = dependency->GetDegree().Get();
		DOUBLE implied_factor =
			std::min(1.0, degree + (1.0 - degree) * implied_sel);
		dependency_scale_factor *= 1.0 / implied_factor;
	}
	GPOS_ASSERT(1.0 <= dependency_scale_factor);

	clauses_attnums->Release();
	estimated_attnums->Release();

	return CDouble(dependency_scale_factor);
}

//---------------------------------------------------------------------------
//	@function:
//		CExtendedStatsProcessor::ApplyCorrelatedStatsToNDistinctCalculation
//
//	@doc:
//		This function is essentially an ORCA version of the selfuncs.c
//		function estimate_multivariate_ndistinct(). It determines the most
//		suitable extended statistic to apply and calculate ndistinct.
//---------------------------------------------------------------------------
bool
CExtendedStatsProcessor::ApplyCorrelatedStatsToNDistinctCalculation(
	CMemoryPool *mp, const IMDExtStatsInfo *md_statsinfo,
	const UlongToIntMap *colid_to_attno_mapping,
	ULongPtrArray *&src_grouping_cols, DOUBLE *ndistinct)
{
	int nmatches;
	OID statOid = InvalidOid;
	const IMDExtStats *stats;
	CBitSet *attnums = nullptr;
	CBitSet *matched = nullptr;

	/* bail out immediately if the table has no extended statistics */
	if (!md_statsinfo || !colid_to_attno_mapping)
	{
		return false;
	}

	attnums = GPOS_NEW(mp) CBitSet(mp);
	for (ULONG ul = 0; ul < src_grouping_cols->Size(); ul++)
	{
		ULONG colid = *(*src_grouping_cols)[ul];

		INT *attnum = colid_to_attno_mapping->Find(&colid);
		if (nullptr == attnum)
		{
			/* no colid -> attno mapping; extended stats are unusable */
			attnums->Release();
			return false;
		}
		if (0 >= *attnum)
		{
			/*
			 * System column: extended statistics never cover it and CBitSet
			 * cannot represent it. Skip just this column; it never enters
			 * 'attnums', so it stays unmatched below and is kept for the
			 * regular per-column ndistinct path.
			 */
			continue;
		}
		attnums->ExchangeSet(*attnum);
	}

	/* look for the ndistinct statistics matching the most vars */
	nmatches = 1; /* we require at least two matches */

	CMDExtStatsInfoArray *md_statsinfo_array =
		md_statsinfo->GetExtStatInfoArray();
	for (ULONG ul = 0; ul < md_statsinfo_array->Size(); ul++)
	{
		CMDExtStatsInfo *info = (*md_statsinfo_array)[ul];
		CBitSet *shared = GPOS_NEW(mp) CBitSet(mp, *attnums);
		int nshared;

		/* skip statistics of other kinds */
		if (info->GetStatKind() != CMDExtStatsInfo::EstatNDistinct)
		{
			shared->Release();
			continue;
		}

		/* compute attnums shared by the vars and the statistics object */
		shared->Intersection(info->GetStatKeys());
		nshared = shared->Size();

		/*
		 * Does this statistics object match more columns than the currently
		 * best object?  If so, use this one instead.
		 *
		 * XXX This should break ties using name of the object, or something
		 * like that, to make the outcome stable.
		 */
		if (nshared > nmatches)
		{
			CRefCount::SafeRelease(matched);

			statOid = info->GetStatOid();
			nmatches = nshared;
			matched = shared;
		}
		else
		{
			shared->Release();
		}
	}

	/* No match? */
	if (statOid == InvalidOid)
	{
		CRefCount::SafeRelease(matched);
		attnums->Release();
		return false;
	}

	const COptCtxt *poctxt = COptCtxt::PoctxtFromTLS();
	CMDAccessor *md_accessor = poctxt->Pmda();

	CMDIdGPDB *pmdid = GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidExtStats, statOid);
	stats = md_accessor->RetrieveExtStats(pmdid);
	pmdid->Release();

	/*
	 * If we have a match, search it for the specific item that matches (there
	 * must be one), and construct the output values.
	 */
	if (stats)
	{
		CMDNDistinct *item = nullptr;

		/* Find the specific item that exactly matches the combination */
		for (ULONG i = 0; i < stats->GetNDistinctList()->Size(); i++)
		{
			CMDNDistinct *tmpitem = (*stats->GetNDistinctList())[i];

			if (tmpitem->GetAttrs()->Equals(matched))
			{
				item = tmpitem;
				break;
			}
		}

		if (nullptr == item)
		{
			/* there should be an item for every attribute combination */
			matched->Release();
			attnums->Release();
			return false;
		}

		/* Form the output varinfo list, keeping only unmatched ones */
		ULongPtrArray *new_src_grouping_cols = GPOS_NEW(mp) ULongPtrArray(mp);
		for (ULONG ul = 0; ul < src_grouping_cols->Size(); ul++)
		{
			ULONG colid = *(*src_grouping_cols)[ul];

			INT *attnum = colid_to_attno_mapping->Find(&colid);
			if (!matched->Get(*attnum))
			{
				new_src_grouping_cols->Append(GPOS_NEW(mp) ULONG(colid));
			}
		}
		src_grouping_cols->Release();
		src_grouping_cols = new_src_grouping_cols;

		matched->Release();
		attnums->Release();

		*ndistinct = item->GetNDistinct().Get();
		return true;
	}

	matched->Release();

	attnums->Release();
	return false;
}
