/*-------------------------------------------------------------------------
 *
 * pg_progsql_shadow.h
 *	  definition of the "progreSQL shadow index registry" system catalog
 *	  (pg_progsql_shadow)
 *
 * Each row maps a unique/primary-key constraint on a partitioned+inherited
 * table to a shadow heap table that enforces cross-partition uniqueness.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_progsql_shadow.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROGSQL_SHADOW_H
#define PG_PROGSQL_SHADOW_H

#include "catalog/genbki.h"
#include "catalog/pg_progsql_shadow_d.h" /* IWYU pragma: export */

/* ----------------
 *		pg_progsql_shadow definition.  cpp turns this into
 *		typedef struct FormData_pg_progsql_shadow
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_progsql_shadow,9200,ProgsqlShadowRelationId)
{
	Oid			oid;			/* oid */

	/* OID of the root (partitioned) relation that owns the constraint */
	Oid			pssrootrelid BKI_LOOKUP(pg_class);

	/* OID of the pg_constraint row for the PK/UNIQUE constraint */
	Oid			pssconid BKI_LOOKUP(pg_constraint);

	/* OID of the shadow heap table */
	Oid			pssshadowid BKI_LOOKUP(pg_class);

	/* OID of the shadow's UNIQUE index on key columns */
	Oid			psskeyidxid BKI_LOOKUP(pg_class);

	/* OID of the shadow's index on pss_child_relid */
	Oid			psschildidxid BKI_LOOKUP(pg_class);
} FormData_pg_progsql_shadow;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_progsql_shadow corresponds to a pointer to a tuple with
 *		the format of pg_progsql_shadow relation.
 * ----------------
 */
typedef FormData_pg_progsql_shadow *Form_pg_progsql_shadow;

DECLARE_UNIQUE_INDEX_PKEY(pg_progsql_shadow_oid_index, 9201, ProgsqlShadowOidIndexId, pg_progsql_shadow, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_progsql_shadow_conid_index, 9202, ProgsqlShadowConidIndexId, pg_progsql_shadow, btree(pssconid oid_ops));
DECLARE_INDEX(pg_progsql_shadow_rootrelid_index, 9203, ProgsqlShadowRootrelidIndexId, pg_progsql_shadow, btree(pssrootrelid oid_ops));

MAKE_SYSCACHE(PROGSQLSHADOWCONID, pg_progsql_shadow_conid_index, 16);


/*
 * prototypes for functions in pg_progsql_shadow.c
 */
extern Oid	CreateProgsqlShadowEntry(Oid rootrelid, Oid conid, Oid shadowid,
									 Oid keyidxid, Oid childidxid);
extern Oid	LookupProgsqlShadow(Oid conid, bool missing_ok);
extern List *GetProgsqlShadowsForRoot(Oid rootrelid);
extern Oid	LookupProgsqlShadowForRoot(Oid rootrelid);
extern bool LookupProgsqlShadowDescForRoot(Oid rootrelid,
										   Oid *shadowOid,
										   Oid *keyIndexOid,
										   Oid *childIndexOid);
extern Oid	CreateProgsqlShadowTable(Oid rootrelid, Oid indexOid, Oid constraintOid);

#endif							/* PG_PROGSQL_SHADOW_H */
