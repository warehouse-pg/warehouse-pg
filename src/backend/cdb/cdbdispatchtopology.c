/*-------------------------------------------------------------------------
 *
 * cdbdispatchtopology.c
 *	  Resolve dispatch addresses from a topology file, bypassing
 *	  gp_segment_configuration row selection.
 *
 * The file is line oriented.  '#' starts a comment line; blank lines are
 * ignored.  Every data line has six whitespace-separated fields:
 *
 *	  content  dbid  hostname  address  port  datadir
 *
 * There must be exactly one line per content, including the coordinator
 * row (content = -1).  The parsed table replaces the rows read from
 * gp_segment_configuration in getCdbComponentInfo(); validation failures
 * are errors, never a silent fallback to the catalog (see
 * cdbdispatchtopology.h).
 *
 * Copyright (c) 2026-Present EnterpriseDB Corporation.
 *
 * src/backend/cdb/cdbdispatchtopology.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>

#include "cdb/cdbdispatchtopology.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"

/* GUC variables; definitions live in guc_gp.c's tables */
char	   *whpg_dispatch_topology_file = NULL;
char	   *whpg_dispatch_topology_state_str = NULL;

/*
 * Last observed parse outcome of this backend.  Deliberately a plain
 * global, not GUC state: it must survive transaction abort so that a
 * failed parse keeps reporting "error" (fail closed) instead of being
 * rolled back together with the erroring transaction.
 */
typedef enum DispatchTopologyState
{
	DISPATCH_TOPOLOGY_UNKNOWN = 0,	/* enabled, but not parsed yet */
	DISPATCH_TOPOLOGY_ACTIVE,
	DISPATCH_TOPOLOGY_ERROR
} DispatchTopologyState;

static DispatchTopologyState topology_state = DISPATCH_TOPOLOGY_UNKNOWN;

/*
 * Worst case for one legal data line, matching the sscanf widths in
 * dispatch_topology_parse(): datadir is a path (MAXPGPATH, %1023s),
 * hostname and address are each capped at 256 — one byte above RFC
 * 1035's 253-byte limit for a fully qualified domain name, and wider
 * than MAXHOSTNAMELEN, which the FTS dump reader itself notes can be
 * smaller than names found in /etc/hosts — plus 64 bytes of slack for
 * content/dbid/port digits, separators and the line ending.  Also
 * sized for the error buffer: fixed text plus the file path.
 */
#define TOPOLOGY_MAX_LINE	(MAXPGPATH + 2 * 256 + 64)

static DispatchTopology *dispatch_topology_parse(const char *path,
												 char *errbuf, size_t errbufsz);

bool
dispatch_topology_enabled(void)
{
	return whpg_dispatch_topology_file != NULL &&
		whpg_dispatch_topology_file[0] != '\0';
}

/*
 * The GUC value is interpreted relative to the data directory; an
 * absolute path is used as-is.
 */
static void
dispatch_topology_resolve_path(char *buf, size_t bufsz)
{
	if (is_absolute_path(whpg_dispatch_topology_file))
		snprintf(buf, bufsz, "%s", whpg_dispatch_topology_file);
	else
		snprintf(buf, bufsz, "%s/%s", DataDir, whpg_dispatch_topology_file);
}

/*
 * Load and validate the topology file, erroring out on any problem.
 * Entries are allocated in the caller's memory context.
 */
DispatchTopology *
dispatch_topology_load(void)
{
	char		path[MAXPGPATH];
	char		errbuf[TOPOLOGY_MAX_LINE];
	DispatchTopology *topology;

	Assert(dispatch_topology_enabled());

	dispatch_topology_resolve_path(path, sizeof(path));

	topology = dispatch_topology_parse(path, errbuf, sizeof(errbuf));
	if (topology == NULL)
	{
		topology_state = DISPATCH_TOPOLOGY_ERROR;
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("dispatch topology file \"%s\" is invalid: %s",
						path, errbuf)));
	}

	topology_state = DISPATCH_TOPOLOGY_ACTIVE;
	return topology;
}

/*
 * Called by the component-table build when the file parsed but failed a
 * cross-check against the catalog, so status keeps reporting "error".
 */
void
dispatch_topology_set_error(void)
{
	topology_state = DISPATCH_TOPOLOGY_ERROR;
}

/*
 * Identity of the topology configuration a component table was built
 * with: empty when the feature is off, otherwise the file path plus a
 * hash of the file bytes.  Compared at transaction start to decide
 * whether the cached component table must be rebuilt.  Content-based
 * on purpose: a same-size file swapped in place must be noticed, and
 * filesystem timestamps are not a reliable witness for that.  The file
 * is a handful of lines, so the read is cheap.  An unreadable file
 * yields a signature that never matches, forcing a rebuild that then
 * reports the real error (fail closed).
 */
char *
dispatch_topology_signature(void)
{
	char		path[MAXPGPATH];
	FILE	   *fd;
	char		buf[8192];
	size_t		nread;
	uint64		hash = 5381;
	uint64		total = 0;

	if (!dispatch_topology_enabled())
		return pstrdup("");

	dispatch_topology_resolve_path(path, sizeof(path));

	fd = AllocateFile(path, "rb");
	if (fd == NULL)
		return psprintf("%s|unreadable", path);

	while ((nread = fread(buf, 1, sizeof(buf), fd)) > 0)
	{
		size_t		i;

		for (i = 0; i < nread; i++)
			hash = hash * 33 + (unsigned char) buf[i];
		total += nread;
	}

	if (ferror(fd))
	{
		FreeFile(fd);
		return psprintf("%s|unreadable", path);
	}
	FreeFile(fd);

	return psprintf("%s|%llu|%llu", path,
					(unsigned long long) total,
					(unsigned long long) hash);
}

bool
dispatch_topology_signature_matches(const char *stored)
{
	char	   *current = dispatch_topology_signature();
	bool		matches;

	matches = (stored != NULL && strcmp(stored, current) == 0);
	pfree(current);
	return matches;
}

/*
 * show_hook for whpg_dispatch_topology_state.  Reads the plain global so
 * the value is not subject to GUC transaction semantics; validates on
 * demand so a fresh session (e.g. a monitoring connection that never
 * dispatched) still reports the truth.
 */
const char *
show_whpg_dispatch_topology_state(void)
{
	if (!dispatch_topology_enabled())
		return "inactive";

	if (topology_state == DISPATCH_TOPOLOGY_UNKNOWN)
	{
		char		path[MAXPGPATH];
		char		errbuf[TOPOLOGY_MAX_LINE];

		dispatch_topology_resolve_path(path, sizeof(path));
		if (dispatch_topology_parse(path, errbuf, sizeof(errbuf)) != NULL)
			topology_state = DISPATCH_TOPOLOGY_ACTIVE;
		else
			topology_state = DISPATCH_TOPOLOGY_ERROR;
	}

	return (topology_state == DISPATCH_TOPOLOGY_ACTIVE) ? "active" : "error";
}

static int
dispatch_topology_entry_cmp(const void *a, const void *b)
{
	const DispatchTopologyEntry *ea = (const DispatchTopologyEntry *) a;
	const DispatchTopologyEntry *eb = (const DispatchTopologyEntry *) b;

	if (ea->content != eb->content)
		return (ea->content < eb->content) ? -1 : 1;
	return 0;
}

/*
 * Parse and validate; returns NULL with a message in errbuf on any
 * problem.  Does not ereport, so it can also back the show_hook.
 */
static DispatchTopology *
dispatch_topology_parse(const char *path, char *errbuf, size_t errbufsz)
{
	FILE	   *fd;
	char		line[TOPOLOGY_MAX_LINE];
	int			lineno = 0;
	int			maxentries = 64;
	bool		has_coordinator = false;
	bool		has_segment = false;
	DispatchTopology *topology;
	int			i;

	fd = AllocateFile(path, "r");
	if (fd == NULL)
	{
		snprintf(errbuf, errbufsz, "could not open file: %m");
		return NULL;
	}

	topology = palloc0(sizeof(DispatchTopology));
	topology->nentries = 0;
	topology->entries = palloc(maxentries * sizeof(DispatchTopologyEntry));

	while (fgets(line, sizeof(line), fd))
	{
		long		content;
		long		dbid;
		long		port;
		char		f_content[16];
		char		f_dbid[16];
		char		f_port[16];
		char		hostname[256];
		char		address[256];
		char		datadir[MAXPGPATH];
		char		extra[8];
		char	   *endptr;
		DispatchTopologyEntry *entry;
		const char *p = line;

		lineno++;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '\n' || *p == '#')
			continue;

		/*
		 * Count the whitespace-separated tokens before sscanf gets to
		 * look: sscanf's %Ns widths split an over-wide token across
		 * adjacent fields, and on a line with a MISSING field such a
		 * split can reassemble into six plausible-looking fields (a
		 * zero-padded 16-digit first token splits into a valid content
		 * and a valid dbid).  Counting tokens judges the line by its
		 * true shape, whatever the token widths; the sscanf field-count
		 * check below stays as the backstop for the six-token cases the
		 * widths still split apart.
		 */
		{
			int			ntokens = 0;
			const char *q = p;

			while (*q != '\0')
			{
				while (*q != '\0' && isspace((unsigned char) *q))
					q++;
				if (*q == '\0')
					break;
				ntokens++;
				while (*q != '\0' && !isspace((unsigned char) *q))
					q++;
			}
			if (ntokens != 6)
			{
				snprintf(errbuf, errbufsz,
						 "line %d: expected 6 fields \"content dbid hostname address port datadir\"",
						 lineno);
				goto fail;
			}
		}

		if (sscanf(p, "%15s %15s %255s %255s %15s %1023s %7s",
				   f_content, f_dbid, hostname, address, f_port, datadir,
				   extra) != 6)
		{
			snprintf(errbuf, errbufsz,
					 "line %d: expected 6 fields \"content dbid hostname address port datadir\"",
					 lineno);
			goto fail;
		}

		/*
		 * The numeric fields go through strtol, never sscanf's %d — %d on
		 * an out-of-range number is undefined behavior — and a malformed
		 * number ("12x") gets a message naming the field instead of the
		 * generic shape error.  The bounds double as the validity rules;
		 * this mirrors the gpqeid hardening on the receiving side
		 * (cdbgang_parse_gpqeid_params).
		 */
		errno = 0;
		content = strtol(f_content, &endptr, 10);
		if (errno != 0 || endptr == f_content || *endptr != '\0' ||
			content < -1 || content > PG_INT16_MAX)
		{
			snprintf(errbuf, errbufsz, "line %d: invalid content \"%s\"",
					 lineno, f_content);
			goto fail;
		}
		errno = 0;
		dbid = strtol(f_dbid, &endptr, 10);
		if (errno != 0 || endptr == f_dbid || *endptr != '\0' ||
			dbid < 1 || dbid > PG_INT16_MAX)
		{
			snprintf(errbuf, errbufsz, "line %d: invalid dbid \"%s\"",
					 lineno, f_dbid);
			goto fail;
		}
		errno = 0;
		port = strtol(f_port, &endptr, 10);
		if (errno != 0 || endptr == f_port || *endptr != '\0' ||
			port < 1 || port > 65535)
		{
			snprintf(errbuf, errbufsz, "line %d: invalid port \"%s\"",
					 lineno, f_port);
			goto fail;
		}
		if (!is_absolute_path(datadir))
		{
			snprintf(errbuf, errbufsz,
					 "line %d: datadir must be an absolute path", lineno);
			goto fail;
		}

		/* exactly one row per content */
		for (i = 0; i < topology->nentries; i++)
		{
			if (topology->entries[i].content == content)
			{
				snprintf(errbuf, errbufsz,
						 "line %d: duplicate entry for content %d",
						 lineno, (int) content);
				goto fail;
			}
		}

		if (topology->nentries == maxentries)
		{
			maxentries *= 2;
			topology->entries = repalloc(topology->entries,
										 maxentries * sizeof(DispatchTopologyEntry));
		}

		entry = &topology->entries[topology->nentries++];
		entry->content = (int16) content;
		entry->dbid = (int16) dbid;
		entry->port = (int32) port;
		entry->hostname = pstrdup(hostname);
		entry->address = pstrdup(address);
		entry->datadir = pstrdup(datadir);

		if (content == -1)
			has_coordinator = true;
		else
			has_segment = true;
	}

	if (ferror(fd))
	{
		snprintf(errbuf, errbufsz, "could not read file: %m");
		goto fail;
	}
	if (!has_coordinator)
	{
		snprintf(errbuf, errbufsz, "missing coordinator row (content -1)");
		goto fail;
	}
	if (!has_segment)
	{
		snprintf(errbuf, errbufsz, "no segment rows");
		goto fail;
	}

	FreeFile(fd);

	qsort(topology->entries, topology->nentries,
		  sizeof(DispatchTopologyEntry), dispatch_topology_entry_cmp);

	return topology;

fail:
	FreeFile(fd);
	return NULL;
}
