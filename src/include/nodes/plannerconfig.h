/*
 * plannerconfig.h
 *
 *  Created on: May 19, 2011
 *      Author: siva
 */

#ifndef PLANNERCONFIG_H_
#define PLANNERCONFIG_H_

/**
 * Planning configuration information
 */
typedef struct PlannerConfig
{
	bool		gp_enable_minmax_optimization;
	bool		gp_enable_multiphase_agg;
	bool		gp_enable_direct_dispatch;

	bool		gp_cte_sharing; /* Indicate whether sharing is to be disabled on any CTEs */

	bool		honor_order_by;

	bool		is_under_subplan; /* True for plan rooted at a subquery which is planned as a subplan */

	bool        force_singleQE; /* True for forcing gather the base rel to singleQE, if it needs a motion */

	bool        may_rescan; /* true means the subquery may be rescanned. */

	/*
	 * NB: keep force_entry and force_entry_rels LAST in this struct, and only
	 * ever append new fields after them.  Appending preserves the byte offsets
	 * of every pre-existing field, which the ABI compliance check requires.
	 * Do not move them into the middle of the struct.
	 *
	 * These handle a correlated SubPlan that scans a coordinator-only (Entry)
	 * catalog joined with a distributed table: it must run on the coordinator,
	 * since the catalog rows only exist there and the correlation parameter
	 * cannot cross a Motion.
	 *
	 * force_entry: force ALL of this query's base rels to Entry (coordinator)
	 * locus.  Set on the SubPlan's own subquery so its whole join executes on
	 * the coordinator (no Broadcast of the Entry table to the segments).
	 *
	 * force_entry_rels: force ONLY these specific base rels (by RT index) to
	 * Entry.  Set on the outer (parent) query for just the rel(s) that supply
	 * the correlation parameter, so the SubPlan runs on the coordinator without
	 * dragging unrelated base rels of that query level to the coordinator too.
	 */
	bool        force_entry;
	struct Bitmapset *force_entry_rels;
} PlannerConfig;

extern PlannerConfig *DefaultPlannerConfig(void);
extern PlannerConfig *CopyPlannerConfig(const PlannerConfig *c1);

#endif /* PLANNERCONFIG_H_ */
