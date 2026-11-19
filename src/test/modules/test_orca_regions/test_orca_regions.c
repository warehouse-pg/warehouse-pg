/*-------------------------------------------------------------------------
 *
 * test_orca_regions.c
 *		Test ORCA's extension hooks (optimizer/orca.h).
 *
 * The test_orca_regions.mode setting picks the regions of the plan ORCA chose
 * that this module's CustomScan executes:
 *
 * off: none.
 *
 * scan: every table scan, as its own leaf. The CustomScan passes the scan's
 * rows on.
 *
 * ipc: a motion right above a table scan, as its own leaf, and the scan
 * below it as an ipc region. The scan's rows cross the motion encoded as
 * (segment, payload) rows, test_orca_regions.batch_rows of them per payload
 * for each segment, and the CustomScan above the motion decodes them. Below
 * a redistribute motion, the segment is the motion's cdbhash of the row, or
 * test_orca_regions.segment if set (to test the executor's checks of it).
 *
 * ext_scan: every table scan of an extension table (below), as a region
 * with pg_filter: the CustomScan reads the table itself, and the executor
 * evaluates the scan's filter and projection over its rows.
 *
 * The remaining modes return malformed regions, which ORCA rejects: the
 * query falls back to the PG planner. span_motion claims a gather motion
 * and the table scan below it as a region over the scan, span_scan an
 * operator above a table scan as a region with no leaves: their interiors
 * hold operators that need translating.
 *
 * Tables of the test_orca_ext table AM stand for tables an extension reads
 * itself. test_orca_regions.ext_relation is what orca_relation_hook says of
 * them, test_orca_regions.ext_rows their row count for
 * orca_relation_size_hook, and test_orca_regions.ext_stats_from the table
 * whose column statistics orca_att_stats_hook gives for them.
 *
 * Only regions of int4 columns are claimed, which keeps the encoding of the
 * ipc rows, and their hashing, trivial.
 *
 * Portions Copyright (c) 2026, EnterpriseDB Corporation
 *
 * IDENTIFICATION
 *		src/test/modules/test_orca_regions/test_orca_regions.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "cdb/cdbhash.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/extensible.h"
#include "optimizer/orca.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

#ifdef USE_ORCA

typedef enum TestMode
{
	MODE_OFF,
	MODE_SCAN,
	MODE_IPC,
	MODE_EXT_SCAN,
	MODE_BAD_VERSION,
	MODE_NO_METHODS,
	MODE_BAD_ROOT,
	MODE_LEAF_OUTSIDE,
	MODE_NESTED_LEAVES,
	MODE_DUPLICATE_ROOT,
	MODE_SORTED_MOTION,
	MODE_SPAN_MOTION,
	MODE_SPAN_SCAN
} TestMode;

static const struct config_enum_entry mode_options[] = {
	{"off", MODE_OFF, false},
	{"scan", MODE_SCAN, false},
	{"ipc", MODE_IPC, false},
	{"ext_scan", MODE_EXT_SCAN, false},
	{"bad_version", MODE_BAD_VERSION, false},
	{"no_methods", MODE_NO_METHODS, false},
	{"bad_root", MODE_BAD_ROOT, false},
	{"leaf_outside", MODE_LEAF_OUTSIDE, false},
	{"nested_leaves", MODE_NESTED_LEAVES, false},
	{"duplicate_root", MODE_DUPLICATE_ROOT, false},
	{"sorted_motion", MODE_SORTED_MOTION, false},
	{"span_motion", MODE_SPAN_MOTION, false},
	{"span_scan", MODE_SPAN_SCAN, false},
	{NULL, 0, false}
};

static int	test_mode = MODE_OFF;

static const struct config_enum_entry relation_options[] = {
	{"none", ORCA_RELATION_NONE, false},
	{"planner", ORCA_RELATION_PLANNER, false},
	{"random", ORCA_RELATION_RANDOM, false},
	{NULL, 0, false}
};

static int	ext_relation = ORCA_RELATION_NONE;
static int	ext_rows = -1;
static char *ext_stats_from = NULL;
static int	batch_rows = 4;
static char *ipc_segment = NULL;

/* the table AM of the tables an extension reads itself */
#define EXT_AM "test_orca_ext"

/* what a region's CustomScan does, its custom_private payload */
#define REGION_PASS "pass"		/* passes its leaf's rows on */
#define REGION_ENCODE "encode"	/* encodes them as (segment, payload) rows:
								 * "encode <rows per payload> <route>" */
#define REGION_DECODE "decode"	/* decodes (segment, payload) rows */
#define REGION_READ "read"		/* reads its leaf's table itself */

/* Metadata side: the extension's tables */

static bool
is_ext_relation(Oid relid)
{
	HeapTuple	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Oid			relam;

	if (!HeapTupleIsValid(tuple))
		return false;
	relam = ((Form_pg_class) GETSTRUCT(tuple))->relam;
	ReleaseSysCache(tuple);
	return OidIsValid(relam) && strcmp(get_am_name(relam), EXT_AM) == 0;
}

/* ORCA runs the statistics hooks with itself off: it isn't reentrant */
static void
check_planner_only(const char *hook)
{
	if (optimizer)
		elog(ERROR, "%s runs with the optimizer on", hook);
}

static OrcaRelation
test_relation(Oid relid)
{
	return is_ext_relation(relid) ? ext_relation : ORCA_RELATION_NONE;
}

static bool
test_relation_size(Oid relid, double *tuples, BlockNumber *pages)
{
	check_planner_only("orca_relation_size_hook");
	if (ext_rows < 0 || !is_ext_relation(relid))
		return false;
	*tuples = ext_rows;
	*pages = ext_rows / 100 + 1;
	return true;
}

static HeapTuple
test_att_stats(Oid relid, AttrNumber attnum)
{
	Oid			from;
	HeapTuple	tuple;

	check_planner_only("orca_att_stats_hook");
	if (ext_stats_from == NULL || ext_stats_from[0] == '\0' ||
		!is_ext_relation(relid))
		return NULL;
	from = RelnameGetRelid(ext_stats_from);
	if (!OidIsValid(from))
		elog(ERROR, "no table \"%s\"", ext_stats_from);
	tuple = SearchSysCacheCopy3(STATRELATTINH, ObjectIdGetDatum(from),
								Int16GetDatum(attnum), BoolGetDatum(false));
	if (HeapTupleIsValid(tuple))
		((Form_pg_statistic) GETSTRUCT(tuple))->starelid = relid;
	return tuple;
}

/* Plan side: finding regions in the DXL */

/*
 * The DXL text from the start tag of the operator number op, in pre-order.
 * The physical operators' element names are their names in opnames, so the
 * op'th start tag naming the next operator in order is op's.
 */
static const char *
find_op(const char *dxl, int op, const char *const *opnames)
{
	const char *p = dxl;
	int			next = 0;

	while ((p = strstr(p, "<dxl:")) != NULL)
	{
		const char *name = p + strlen("<dxl:");
		size_t		len = strlen(opnames[next]);

		if (strncmp(name, opnames[next], len) == 0 &&
			(name[len] == ' ' || name[len] == '>' || name[len] == '/'))
		{
			if (next == op)
				return p;
			next++;
		}
		p = name;
	}
	return NULL;
}

/* the value of attribute attr in the element at p, palloc'd */
static char *
attr_value(const char *p, const char *attr)
{
	const char *end = strchr(p, '>');
	char		key[64];
	const char *v;

	snprintf(key, sizeof(key), " %s=\"", attr);
	v = strstr(p, key);
	if (v == NULL || v > end)
		return NULL;
	v += strlen(key);
	return pnstrdup(v, strchr(v, '"') - v);
}

/*
 * The output columns of the operator whose element starts at p, from its
 * project list. Returns false unless they are all plain int4 columns.
 */
static bool
op_columns(const char *p, int *ncols, OrcaPlanRegionCol **cols)
{
	const char *list = strstr(p, "<dxl:ProjList>");
	const char *end = list ? strstr(list, "</dxl:ProjList>") : NULL;
	const char *elem;
	int			n = 0;

	if (end == NULL)
		return false;
	*cols = palloc0(sizeof(OrcaPlanRegionCol) * 64);
	for (elem = strstr(list, "<dxl:ProjElem "); elem != NULL && elem < end;
		 elem = strstr(elem + 1, "<dxl:ProjElem "))
	{
		const char *ident = strchr(elem, '>') + 1;
		char	   *type;

		if (n == 64 || strncmp(ident, "<dxl:Ident ", 11) != 0)
			return false;
		type = attr_value(ident, "TypeMdid");
		if (type == NULL || strcmp(type, "0.23.1.0") != 0)
			return false;
		(*cols)[n].colid = (uint32) atoi(attr_value(elem, "ColId"));
		(*cols)[n].type = INT4OID;
		(*cols)[n].typmod = -1;
		(*cols)[n].name = attr_value(elem, "Alias");
		n++;
	}
	*ncols = n;
	return n > 0;
}

/*
 * The user columns of the table the scan whose element starts at p reads,
 * from its table descriptor, if it is an extension table of int4 columns.
 */
static bool
ext_scan_columns(const char *p, int *ncols, OrcaPlanRegionCol **cols)
{
	const char *close = p + 1;
	const char *descr = NULL;
	const char *end;
	int			depth = 1;

	/*
	 * The scan's own descriptor is its last child: its filter may hold
	 * subplans scanning other tables
	 */
	while (depth > 0)
	{
		const char *open = strstr(close, "<dxl:TableScan");

		/* the element, which may have no attributes */
		while (open != NULL && open[14] != ' ' && open[14] != '>')
			open = strstr(open + 1, "<dxl:TableScan");

		close = strstr(close, "</dxl:TableScan>");
		if (close == NULL)
			return false;
		if (open != NULL && open < close)
		{
			depth++;
			close = open + 1;
		}
		else if (--depth > 0)
			close++;
	}
	for (const char *d = strstr(p, "<dxl:TableDescriptor "); d != NULL && d < close;
		 d = strstr(d + 1, "<dxl:TableDescriptor "))
		descr = d;
	end = descr ? strstr(descr, "</dxl:Columns>") : NULL;
	char	   *mdid;
	const char *col;
	int			n = 0;

	if (end == NULL)
		return false;
	/* a relation's mdid is <type>.<oid>.<major>.<minor> */
	mdid = attr_value(descr, "Mdid");
	if (mdid == NULL || strchr(mdid, '.') == NULL ||
		!is_ext_relation((Oid) strtoul(strchr(mdid, '.') + 1, NULL, 10)))
		return false;
	*cols = palloc0(sizeof(OrcaPlanRegionCol) * 64);
	for (col = strstr(descr, "<dxl:Column "); col != NULL && col < end;
		 col = strstr(col + 1, "<dxl:Column "))
	{
		char	   *type = attr_value(col, "TypeMdid");

		if (atoi(attr_value(col, "Attno")) <= 0)
			continue;
		if (n == 64 || type == NULL || strcmp(type, "0.23.1.0") != 0)
			return false;
		(*cols)[n].colid = (uint32) atoi(attr_value(col, "ColId"));
		(*cols)[n].type = INT4OID;
		(*cols)[n].typmod = -1;
		(*cols)[n].name = attr_value(col, "ColName");
		n++;
	}
	*ncols = n;
	return n > 0;
}

static bool
is_motion(const char *opname)
{
	return strcmp(opname, "GatherMotion") == 0 ||
		strcmp(opname, "BroadcastMotion") == 0 ||
		strcmp(opname, "RedistributeMotion") == 0 ||
		strcmp(opname, "RandomMotion") == 0;
}

/* the oid of a mdid <type>.<oid>.<major>.<minor> */
static Oid
mdid_oid(const char *mdid)
{
	return mdid && strchr(mdid, '.') ?
		(Oid) strtoul(strchr(mdid, '.') + 1, NULL, 10) : InvalidOid;
}

/*
 * The payload of the ipc region of the table scan at scan below the motion
 * at motion, with its route: how it computes each row's segment. "gather"
 * or "broadcast" (any segment), "random <segments>", "fixed <segment>" (the
 * test_orca_regions.segment setting), or "hash <segments> <attno>:<hash
 * function>..." for a redistribute motion whose hash keys are int4 columns
 * of the scan. NULL if it is none of those.
 */
static const char *
encode_spec(const char *motion, const char *scan, const char *motion_name)
{
	StringInfoData spec;
	const char *segs = attr_value(motion, "OutputSegments");
	int			nsegs = 1;

	initStringInfo(&spec);
	appendStringInfo(&spec, "%s %d ", REGION_ENCODE, batch_rows);
	for (const char *c = segs; c && *c; c++)
		nsegs += *c == ',';
	if (strcmp(motion_name, "GatherMotion") == 0)
		appendStringInfoString(&spec, "gather");
	else if (strcmp(motion_name, "BroadcastMotion") == 0)
		appendStringInfoString(&spec, "broadcast");
	else if (ipc_segment != NULL && ipc_segment[0] != '\0')
		appendStringInfo(&spec, "fixed %d", atoi(ipc_segment));
	else if (strcmp(motion_name, "RandomMotion") == 0)
		appendStringInfo(&spec, "random %d", nsegs);
	else
	{
		/* the motion's own hash keys, before its child's element */
		const char *list = strstr(motion, "<dxl:HashExprList>");
		const char *end = list ? strstr(list, "</dxl:HashExprList>") : NULL;
		int			ncols;
		OrcaPlanRegionCol *cols;

		if (end == NULL || end > scan || !op_columns(scan, &ncols, &cols))
			return NULL;
		appendStringInfo(&spec, "hash %d", nsegs);
		for (const char *key = strstr(list, "<dxl:HashExpr ");
			 key != NULL && key < end; key = strstr(key + 1, "<dxl:HashExpr "))
		{
			const char *ident = strchr(key, '>') + 1;
			char	   *colid;
			int			attno = 0;

			if (strncmp(ident, "<dxl:Ident ", 11) != 0 ||
				mdid_oid(attr_value(ident, "TypeMdid")) != INT4OID)
				return NULL;
			colid = attr_value(ident, "ColId");
			for (int i = 0; i < ncols; i++)
				if (cols[i].colid == (uint32) atoi(colid))
					attno = i + 1;
			if (attno == 0)
				return NULL;
			appendStringInfo(&spec, " %d:%u", attno,
							 cdb_hashproc_in_opfamily(mdid_oid(attr_value(key, "Opfamily")),
													  INT4OID));
		}
	}
	return spec.data;
}

static CustomScanMethods region_scan_methods;

static OrcaPlanRegion *
add_region(OrcaPlanRegion *regions, int *nregions, int root, const char *dxl)
{
	OrcaPlanRegion *region = &regions[(*nregions)++];
	int		   *leaves = palloc(sizeof(int));

	leaves[0] = root;
	region->root = root;
	region->dxl = dxl;
	region->nleaves = 1;
	region->leaves = leaves;
	return region;
}

static const OrcaPlanRegions *
test_plan_regions(const char *plan_dxl, int nops, const char *const *opnames)
{
	OrcaPlanRegions *result;
	OrcaPlanRegion *regions;
	int			nregions = 0;

	if (test_mode == MODE_OFF)
		return NULL;

	result = palloc0(sizeof(OrcaPlanRegions));
	result->version = ORCA_HOOKS_ABI_VERSION;
	result->methods = &region_scan_methods;
	/* at most two regions per operator */
	regions = palloc0(sizeof(OrcaPlanRegion) * 2 * nops);
	result->regions = regions;

	for (int op = 0; op < nops; op++)
	{
		const char *p = find_op(plan_dxl, op, opnames);
		int			ncols;
		OrcaPlanRegionCol *cols;
		OrcaPlanRegion *region;

		if (p == NULL)
			elog(ERROR, "operator %d (%s) not in the plan's DXL", op,
				 opnames[op]);

		switch (test_mode)
		{
			case MODE_SCAN:
				if (strcmp(opnames[op], "TableScan") == 0 &&
					op_columns(p, &ncols, &cols))
				{
					region = add_region(regions, &nregions, op, REGION_PASS);
					region->ncols = ncols;
					region->cols = cols;
				}
				break;

			case MODE_IPC:
				{
					const char *spec;

					if (op + 1 < nops && is_motion(opnames[op]) &&
						strcmp(opnames[op + 1], "TableScan") == 0 &&
						op_columns(p, &ncols, &cols) &&
						(spec = encode_spec(p, find_op(plan_dxl, op + 1, opnames),
											opnames[op])) != NULL)
					{
						region = add_region(regions, &nregions, op, REGION_DECODE);
						region->ncols = ncols;
						region->cols = cols;
						region = add_region(regions, &nregions, op + 1, spec);
						region->ipc = true;
					}
					break;
				}

			case MODE_EXT_SCAN:
				if (strcmp(opnames[op], "TableScan") == 0 &&
					ext_scan_columns(p, &ncols, &cols))
				{
					region = add_region(regions, &nregions, op, REGION_READ);
					region->ncols = ncols;
					region->cols = cols;
					region->pg_filter = true;
				}
				break;

			case MODE_SORTED_MOTION:
				/* an ipc region below a gather that merges sorted rows */
				if (op + 1 < nops && strcmp(opnames[op], "GatherMotion") == 0 &&
					strncmp(strstr(p, "<dxl:SortingColumnList"),
							"<dxl:SortingColumnList/>", 24) != 0)
				{
					region = add_region(regions, &nregions, op + 1,
										REGION_ENCODE);
					region->ipc = true;
				}
				break;

			case MODE_SPAN_MOTION:
				/* the gather is in the region, not a leaf */
				if (op + 1 < nops && strcmp(opnames[op], "GatherMotion") == 0 &&
					strcmp(opnames[op + 1], "TableScan") == 0 &&
					op_columns(p, &ncols, &cols))
				{
					region = add_region(regions, &nregions, op, REGION_PASS);
					((int *) region->leaves)[0] = op + 1;
					region->ncols = ncols;
					region->cols = cols;
				}
				break;

			case MODE_SPAN_SCAN:
				/* the scan is in the region, not a leaf */
				if (op + 1 < nops && !is_motion(opnames[op]) &&
					strcmp(opnames[op + 1], "TableScan") == 0 &&
					op_columns(p, &ncols, &cols))
				{
					region = add_region(regions, &nregions, op, REGION_PASS);
					region->nleaves = 0;
					region->ncols = ncols;
					region->cols = cols;
				}
				break;

			default:
				break;
		}
	}

	if (nops > 0 && test_mode >= MODE_BAD_VERSION &&
		test_mode < MODE_SORTED_MOTION)
	{
		OrcaPlanRegionCol *col = palloc0(sizeof(OrcaPlanRegionCol));
		OrcaPlanRegion *region;

		col->type = INT4OID;
		col->typmod = -1;
		col->name = "c";
		region = add_region(regions, &nregions, 0, REGION_PASS);
		region->ncols = 1;
		region->cols = col;
		switch (test_mode)
		{
			case MODE_BAD_VERSION:
				result->version = ORCA_HOOKS_ABI_VERSION + 1;
				break;
			case MODE_NO_METHODS:
				result->methods = NULL;
				break;
			case MODE_BAD_ROOT:
				region->root = nops;
				break;
			case MODE_LEAF_OUTSIDE:
				{
					/* the root's leaf is outside its subtree */
					int		   *leaves = palloc(sizeof(int));

					region->root = nops - 1;
					leaves[0] = 0;
					region->leaves = leaves;
					break;
				}
			case MODE_NESTED_LEAVES:
				{
					/* the root, and a leaf in its subtree */
					int		   *leaves = palloc(2 * sizeof(int));

					leaves[0] = 0;
					leaves[1] = nops - 1;
					region->nleaves = 2;
					region->leaves = leaves;
					break;
				}
			case MODE_DUPLICATE_ROOT:
				add_region(regions, &nregions, 0, REGION_PASS)->ncols = 1;
				regions[nregions - 1].cols = col;
				break;
			default:
				break;
		}
	}

	result->nregions = nregions;
	return result;
}

/* Execution side: the regions' CustomScan */

typedef struct RegionScanState
{
	CustomScanState css;
	const char *what;			/* REGION_PASS, _ENCODE, _DECODE or _READ */
	TableScanDesc scan;			/* REGION_READ's scan of its table */
	TupleTableSlot *table_slot;
	AttrNumber *attnos;			/* the table's attribute of each column */

	/* REGION_ENCODE: its route, and the rows it holds for each segment */
	const char *route;			/* "gather", "hash", ... (encode_spec) */
	int			batch;			/* rows per payload */
	int			nsegs;			/* the segments it sends to */
	int			fixed;			/* the segment of "fixed" */
	int			nkeys;			/* the hash keys of "hash" */
	AttrNumber *keys;
	CdbHash    *hash;
	uint32		next_random;
	StringInfoData *bufs;		/* one per segment, one for a gather or
								 * broadcast */
	int		   *nrows;
	int			nbufs;
	bool		done;			/* its leaf has no more rows */
	int			flushed;		/* the buffers sent after that */

	/* REGION_DECODE: the payload it reads rows from */
	const char *pos;
	int			left;
} RegionScanState;

/* each column of an encoded row: a null flag and an int4 */
#define ENCODED_COL (1 + sizeof(int32))

static void
encode_reset(RegionScanState *state)
{
	for (int i = 0; i < state->nbufs; i++)
	{
		resetStringInfo(&state->bufs[i]);
		state->nrows[i] = 0;
	}
	state->done = false;
	state->flushed = 0;
	state->next_random = 0;
}

/* parses the region's "encode <rows> <route>" payload */
static void
encode_begin(RegionScanState *state, const char *spec)
{
	char	   *copy = pstrdup(spec);
	char	   *save;
	char	   *tok;

	strtok_r(copy, " ", &save);
	state->batch = atoi(strtok_r(NULL, " ", &save));
	state->route = pstrdup(strtok_r(NULL, " ", &save));
	state->nsegs = 1;
	state->nbufs = 1;
	if (strcmp(state->route, "fixed") == 0)
		state->fixed = atoi(strtok_r(NULL, " ", &save));
	else if (strcmp(state->route, "random") == 0 ||
			 strcmp(state->route, "hash") == 0)
	{
		state->nsegs = atoi(strtok_r(NULL, " ", &save));
		state->nbufs = state->nsegs;
	}
	if (strcmp(state->route, "hash") == 0)
	{
		Oid		   *funcs = palloc(sizeof(Oid) * 64);

		state->keys = palloc(sizeof(AttrNumber) * 64);
		while ((tok = strtok_r(NULL, " ", &save)) != NULL)
		{
			state->keys[state->nkeys] = atoi(tok);
			funcs[state->nkeys++] = (Oid) strtoul(strchr(tok, ':') + 1, NULL, 10);
		}
		state->hash = makeCdbHash(state->nsegs, state->nkeys, funcs);
	}
	state->bufs = palloc0(sizeof(StringInfoData) * state->nbufs);
	state->nrows = palloc0(sizeof(int) * state->nbufs);
	for (int i = 0; i < state->nbufs; i++)
		initStringInfo(&state->bufs[i]);
	encode_reset(state);
}

/* the buffer, and the segment, of the row in the slot */
static int
encode_target(RegionScanState *state, TupleTableSlot *in, int *segment)
{
	if (strcmp(state->route, "hash") == 0)
	{
		cdbhashinit(state->hash);
		for (int i = 0; i < state->nkeys; i++)
			cdbhash(state->hash, i + 1, in->tts_values[state->keys[i] - 1],
					in->tts_isnull[state->keys[i] - 1]);
		*segment = cdbhashreduce(state->hash);
		return *segment;
	}
	if (strcmp(state->route, "random") == 0)
	{
		*segment = state->next_random++ % state->nsegs;
		return *segment;
	}
	*segment = strcmp(state->route, "fixed") == 0 ? state->fixed : 0;
	return 0;
}

/* the (segment, payload) row of the buffer's rows */
static TupleTableSlot *
encode_send(RegionScanState *state, int buf, int segment, TupleTableSlot *out)
{
	bytea	   *payload = palloc(VARHDRSZ + state->bufs[buf].len);

	SET_VARSIZE(payload, VARHDRSZ + state->bufs[buf].len);
	memcpy(VARDATA(payload), state->bufs[buf].data, state->bufs[buf].len);
	resetStringInfo(&state->bufs[buf]);
	state->nrows[buf] = 0;
	out->tts_values[0] = Int32GetDatum(segment);
	out->tts_isnull[0] = false;
	out->tts_values[1] = PointerGetDatum(payload);
	out->tts_isnull[1] = false;
	return ExecStoreVirtualTuple(out);
}

/* the next payload, of a full buffer, or after the last row of a buffer */
static TupleTableSlot *
encode_next(RegionScanState *state, TupleTableSlot *out)
{
	while (!state->done)
	{
		TupleTableSlot *in = ExecProcNode(linitial(state->css.custom_ps));
		int			segment;
		int			buf;

		if (TupIsNull(in))
		{
			state->done = true;
			break;
		}
		slot_getallattrs(in);
		buf = encode_target(state, in, &segment);
		for (int i = 0; i < in->tts_tupleDescriptor->natts; i++)
		{
			int32		value = in->tts_isnull[i] ? 0 :
				DatumGetInt32(in->tts_values[i]);

			appendStringInfoChar(&state->bufs[buf], in->tts_isnull[i]);
			appendBinaryStringInfo(&state->bufs[buf], (char *) &value,
								   sizeof(int32));
		}
		if (++state->nrows[buf] == state->batch)
			return encode_send(state, buf, segment, out);
	}
	for (; state->flushed < state->nbufs; state->flushed++)
	{
		int			buf = state->flushed;

		if (state->nrows[buf] > 0)
			return encode_send(state, buf,
							   state->nbufs > 1 ? buf :
							   strcmp(state->route, "fixed") == 0 ? state->fixed : 0,
							   out);
	}
	return out;
}

/* the next row of the payloads it reads */
static TupleTableSlot *
decode_next(RegionScanState *state, TupleTableSlot *out)
{
	int			nout = out->tts_tupleDescriptor->natts;

	if (state->left == 0)
	{
		TupleTableSlot *in = ExecProcNode(linitial(state->css.custom_ps));
		bytea	   *payload;
		int			len;

		if (TupIsNull(in))
			return out;
		slot_getallattrs(in);
		payload = DatumGetByteaPP(in->tts_values[1]);
		len = VARSIZE_ANY_EXHDR(payload);
		if (len == 0 || len % (nout * ENCODED_COL) != 0)
			elog(ERROR, "malformed region payload");
		state->pos = VARDATA_ANY(payload);
		state->left = len / (nout * ENCODED_COL);
	}
	for (int i = 0; i < nout; i++)
	{
		int32		value;

		out->tts_isnull[i] = *state->pos++;
		memcpy(&value, state->pos, sizeof(int32));
		state->pos += sizeof(int32);
		out->tts_values[i] = Int32GetDatum(value);
	}
	state->left--;
	return ExecStoreVirtualTuple(out);
}

static void
region_begin(CustomScanState *node, EState *estate, int eflags)
{
	RegionScanState *state = (RegionScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	ListCell   *lc;

	if (strcmp(state->what, REGION_READ) == 0)
	{
		/* the leaf only names the table */
		Scan	   *leaf = linitial(cscan->custom_plans);
		Relation	rel = ExecOpenScanRelation(estate, leaf->scanrelid, eflags);
		int			i = 0;

		/*
		 * The region's columns, those of the table the query uses: the scan
		 * tuple's entries are Vars of them
		 */
		state->attnos = palloc(sizeof(AttrNumber) *
							   list_length(cscan->custom_scan_tlist));
		foreach(lc, cscan->custom_scan_tlist)
			state->attnos[i++] = castNode(Var, ((TargetEntry *) lfirst(lc))->expr)->varattno;

		state->table_slot = table_slot_create(rel, &estate->es_tupleTable);
		if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
			state->scan = table_beginscan(rel, estate->es_snapshot, 0, NULL);
		return;
	}
	if (strcmp(state->what, REGION_ENCODE) == 0)
		encode_begin(state, strVal(linitial(cscan->custom_private)));
	foreach(lc, cscan->custom_plans)
		node->custom_ps = lappend(node->custom_ps,
								  ExecInitNode(lfirst(lc), estate, eflags));
}

/* the next row of the region, in its scan tuple slot */
static TupleTableSlot *
region_next(ScanState *ss)
{
	RegionScanState *state = (RegionScanState *) ss;
	TupleTableSlot *out = ss->ss_ScanTupleSlot;
	TupleTableSlot *in;
	int			natts;

	ExecClearTuple(out);
	if (state->scan != NULL)
	{
		/* this segment's rows of the table */
		if (!table_scan_getnextslot(state->scan, ForwardScanDirection,
									state->table_slot))
			return out;
		slot_getallattrs(state->table_slot);
		for (int i = 0; i < out->tts_tupleDescriptor->natts; i++)
		{
			out->tts_values[i] = state->table_slot->tts_values[state->attnos[i] - 1];
			out->tts_isnull[i] = state->table_slot->tts_isnull[state->attnos[i] - 1];
		}
		return ExecStoreVirtualTuple(out);
	}
	if (strcmp(state->what, REGION_ENCODE) == 0)
		return encode_next(state, out);
	if (strcmp(state->what, REGION_DECODE) == 0)
		return decode_next(state, out);

	/* REGION_PASS */
	in = ExecProcNode(linitial(state->css.custom_ps));
	if (TupIsNull(in))
		return out;
	slot_getallattrs(in);
	natts = in->tts_tupleDescriptor->natts;
	Assert(natts == out->tts_tupleDescriptor->natts);
	memcpy(out->tts_values, in->tts_values, natts * sizeof(Datum));
	memcpy(out->tts_isnull, in->tts_isnull, natts * sizeof(bool));
	return ExecStoreVirtualTuple(out);
}

static bool
region_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
region_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, region_next, region_recheck);
}

static void
region_end(CustomScanState *node)
{
	RegionScanState *state = (RegionScanState *) node;
	ListCell   *lc;

	if (state->scan != NULL)
		table_endscan(state->scan);
	foreach(lc, node->custom_ps)
		ExecEndNode(lfirst(lc));
}

static void
region_rescan(CustomScanState *node)
{
	RegionScanState *state = (RegionScanState *) node;
	ListCell   *lc;

	if (state->scan != NULL)
		table_rescan(state->scan, NULL);
	if (state->bufs != NULL)
		encode_reset(state);
	state->left = 0;
	foreach(lc, node->custom_ps)
		ExecReScan(lfirst(lc));
}

static void
region_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ExplainPropertyText("Region", ((RegionScanState *) node)->what, es);
}

static CustomExecMethods region_exec_methods = {
	.CustomName = "TestOrcaRegion",
	.BeginCustomScan = region_begin,
	.ExecCustomScan = region_exec,
	.EndCustomScan = region_end,
	.ReScanCustomScan = region_rescan,
	.ExplainCustomScan = region_explain,
};

static Node *
region_create_state(CustomScan *cscan)
{
	RegionScanState *state = palloc0(sizeof(RegionScanState));
	const char *payload = strVal(linitial(cscan->custom_private));

	NodeSetTag(state, T_CustomScanState);
	state->css.methods = &region_exec_methods;
	/* its first word */
	state->what = pnstrdup(payload, strcspn(payload, " "));
	return (Node *) state;
}

static CustomScanMethods region_scan_methods = {
	.CustomName = "TestOrcaRegion",
	.CreateCustomScanState = region_create_state,
};

#endif							/* USE_ORCA */

void
_PG_init(void)
{
#ifdef USE_ORCA
	DefineCustomEnumVariable("test_orca_regions.mode",
							 "Which regions of ORCA's plans to claim.",
							 NULL,
							 &test_mode,
							 MODE_OFF,
							 mode_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("test_orca_regions.ext_relation",
							 "What orca_relation_hook says of extension tables.",
							 NULL,
							 &ext_relation,
							 ORCA_RELATION_NONE,
							 relation_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("test_orca_regions.ext_rows",
							"The row count of extension tables, or -1.",
							NULL,
							&ext_rows,
							-1,
							-1, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
	DefineCustomStringVariable("test_orca_regions.ext_stats_from",
							   "The table whose column statistics extension tables have.",
							   NULL,
							   &ext_stats_from,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);
	DefineCustomIntVariable("test_orca_regions.batch_rows",
							"The rows of each payload of the ipc regions.",
							NULL,
							&batch_rows,
							4,
							1, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
	DefineCustomStringVariable("test_orca_regions.segment",
							   "The segment of the ipc regions' rows below a redistribute motion, if not their cdbhash.",
							   NULL,
							   &ipc_segment,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);
	RegisterCustomScanMethods(&region_scan_methods);
	orca_plan_regions_hook = test_plan_regions;
	orca_relation_hook = test_relation;
	orca_relation_size_hook = test_relation_size;
	orca_att_stats_hook = test_att_stats;
#endif
}
