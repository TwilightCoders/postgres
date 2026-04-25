/*-------------------------------------------------------------------------
 *
 * progsql_shadow_scan.h
 *	  ProgreSQL CustomScan that routes point lookups on a partitioned+
 *	  inherited table through the shadow key index.
 *
 * The shadow index maps a PK/UNIQUE key value to the pss_child_relid of
 * the partition that owns the row, enabling O(log n) lookups when the
 * WHERE clause equates a key column to a constant or parameter (rather
 * than the partition key).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/progsql_shadow_scan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROGSQL_SHADOW_SCAN_H
#define PROGSQL_SHADOW_SCAN_H

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "optimizer/planmain.h"

extern void progsql_shadow_scan_init(void);
extern void progsql_add_shadow_paths(PlannerInfo *root, RelOptInfo *rel,
									 Index rti, RangeTblEntry *rte);

#endif							/* PROGSQL_SHADOW_SCAN_H */
