/*-------------------------------------------------------------------------
 *
 * binary_upgrade.h
 *	  variables used for binary upgrades
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/binary_upgrade.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BINARY_UPGRADE_H
#define BINARY_UPGRADE_H

#include "access/xlogdefs.h"

/*
 * Per-attribute versions, as they were in the old cluster, for a row-type
 * bearing relation.  natts is 0 if the relation had no versions there.
 */
typedef struct BinaryUpgradeAttributeVersions
{
	Oid			relid;
	int			natts;
	XLogRecPtr	versions[FLEXIBLE_ARRAY_MEMBER];
} BinaryUpgradeAttributeVersions;

extern PGDLLIMPORT Oid binary_upgrade_next_pg_type_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_array_pg_type_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_toast_pg_type_oid;

extern PGDLLIMPORT Oid binary_upgrade_next_heap_pg_class_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_index_pg_class_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_toast_pg_class_oid;

extern PGDLLIMPORT Oid binary_upgrade_next_pg_enum_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_pg_authid_oid;

extern void AddBinaryUpgradeAttributeVersions(Oid relid, int natts,
								  const XLogRecPtr *versions);

#endif   /* BINARY_UPGRADE_H */
