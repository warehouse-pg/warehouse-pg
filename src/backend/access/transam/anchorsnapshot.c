/*-------------------------------------------------------------------------
 *
 * anchorsnapshot.c
 *	  Anchor snapshots for hot-standby reads pinned to a restore point.
 *
 * A restore point is written under the two-phase commit lock, so the
 * record every node of the cluster replays marks one cluster-wide
 * instant.  When the startup process of a hot standby replays such a
 * record it exports the node's xid snapshot at that instant: the xid set
 * goes into a file under pg_anchor_snapshots/ named after the restore
 * point (before the node publishes the record's replay position, so a
 * replay position at or past the record implies the anchor is registered
 * or was refused), and {restore point name, xmin} is registered in a fixed-capacity
 * shared-memory registry.  A backend that later imports the anchor named
 * by whpg_hot_standby_anchor_name sees, on every node, the same cut.
 *
 * Everything in this file that runs in the startup process degrades to
 * "no anchor for this restore point", never to "no standby": an ERROR
 * raised by the startup process is FATAL and a transient ENOSPC would
 * otherwise crash-loop the server, so every failure on the export path
 * is a WARNING and a skip.  For the same reason the export never calls
 * GetSnapshotData(): it reads KnownAssignedXids through a small
 * procarray helper instead.
 *
 * Lifecycle: at every server start the registry is rebuilt from the one
 * file the anchor-name GUC points at (every other file is swept), and
 * only once replay has confirmed that anchor's restore-point record: an
 * immediate shutdown or a crash restarts redo before the anchor, and hot
 * standby can open for connections before that record is met again (it
 * opens at minRecoveryPoint, which lags the anchor whenever no page was
 * written back after it), so an anchor registered at start could be
 * imported against a data state that predates it.  A reload that names a
 * registered anchor retires every anchor registered before it, and so does
 * the export of the name the GUC already carries; a full registry evicts
 * its oldest unpublished anchor (only the published one is ever imported);
 * promotion clears the registry and the directory before the shared
 * recovery state flips to done.  There are no reference counts and no
 * age-based expiry.
 *
 * Only the startup process writes the registry; backends read it.  The
 * spinlock exists for the readers' benefit (a consistent copy of an
 * entry) and every critical section is a bounded scan without IO.
 *
 * Copyright (c) 2026-Present EnterpriseDB Corporation.
 *
 * src/backend/access/transam/anchorsnapshot.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/anchorsnapshot.h"
#include "access/timeline.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "common/string.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/faultinjector.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/* GUC variables; the entries live in guc_gp.c */
int			whpg_max_anchor_snapshots = 64;
char	   *whpg_hot_standby_anchor_name = NULL;

typedef struct AnchorRegistryEntry
{
	char		rp_name[MAXFNAMELEN];	/* key: the restore point name */
	TransactionId xmin;			/* the anchor's xmin */
	uint64		ordinal;		/* registration order, never exposed */
	bool		valid;
} AnchorRegistryEntry;

typedef struct AnchorRegistryData
{
	slock_t		lock;
	int			capacity;		/* whpg_max_anchor_snapshots at postmaster start */
	uint64		next_ordinal;
	AnchorRegistryEntry entries[FLEXIBLE_ARRAY_MEMBER];
} AnchorRegistryData;

static AnchorRegistryData *anchorRegistry = NULL;

/*
 * The xid array the export fills; allocated once per startup process on
 * first use (GetMaxSnapshotSubxidCount() entries) and kept.
 */
static TransactionId *exportXids = NULL;

/*
 * The anchor name the startup process last acted on: AnchorSnapshotStartup
 * records the boot value, AnchorSnapshotOnConfigReload compares against it.
 */
static char lastSeenAnchorName[MAXFNAMELEN];
static bool lastSeenAnchorNameSet = false;

/*
 * The anchor whose file survived a restart but whose restore-point record
 * lies past this start's redo point.  It is registered only when replay
 * meets that very record again (same name, timeline and end LSN); until
 * then the node has no anchor.  Startup process only.
 */
static bool pendingSet = false;
static char pendingName[MAXFNAMELEN];
static TimeLineID pendingTLI;
static XLogRecPtr pendingLSN;
static TransactionId pendingXmin;

/* Names collected under the spinlock for retirement; sized to capacity. */
static char *retireNames = NULL;

/* Version of the on-disk grammar; bumped when a field is added. */
#define ANCHOR_FILE_FORMAT	1

static bool anchorNameIsValid(const char *name);
static void anchorFilePath(char *path, size_t len, const char *name, bool tmp);
static bool writeAnchorFile(const char *name, TimeLineID tli, XLogRecPtr lsn,
							TransactionId xmin, TransactionId xmax,
							const TransactionId *xids, int nxids,
							bool suboverflowed);
static bool parseAnchorFile(const char *name, TimeLineID *tli,
							XLogRecPtr *lsn, TransactionId *xmin);
static void sweepAnchorFiles(const char *keep);
typedef enum RegisterResult
{
	REGISTER_OK,
	REGISTER_DUPLICATE,			/* the name is already registered */
	REGISTER_FULL				/* no free slot */
} RegisterResult;

static RegisterResult registerAnchor(const char *name, TransactionId xmin);
static bool evictOldestUnpublished(const char *newname);
static bool applyPublication(const char *name);
static AnchorRegistryEntry *findEntryLocked(const char *name);


/* ------------------------------------------------------------------
 * Shared memory
 * ------------------------------------------------------------------
 */

static inline bool
registryEnabled(void)
{
	return anchorRegistry != NULL && anchorRegistry->capacity > 0;
}

Size
AnchorRegistryShmemSize(void)
{
	Size		size;

	size = offsetof(AnchorRegistryData, entries);
	size = add_size(size, mul_size(sizeof(AnchorRegistryEntry),
								   Max(whpg_max_anchor_snapshots, 0)));
	return size;
}

void
AnchorRegistryShmemInit(void)
{
	bool		found;

	anchorRegistry = (AnchorRegistryData *)
		ShmemInitStruct("Anchor Snapshot Registry",
						AnchorRegistryShmemSize(), &found);
	if (!found)
	{
		SpinLockInit(&anchorRegistry->lock);
		anchorRegistry->capacity = Max(whpg_max_anchor_snapshots, 0);
		anchorRegistry->next_ordinal = 1;
		if (anchorRegistry->capacity > 0)
			memset(anchorRegistry->entries, 0,
				   sizeof(AnchorRegistryEntry) * anchorRegistry->capacity);
	}
}

/* Caller holds the spinlock. */
static AnchorRegistryEntry *
findEntryLocked(const char *name)
{
	int			i;

	for (i = 0; i < anchorRegistry->capacity; i++)
	{
		AnchorRegistryEntry *e = &anchorRegistry->entries[i];

		if (e->valid && strcmp(e->rp_name, name) == 0)
			return e;
	}
	return NULL;
}

/*
 * Register {name, xmin}.  Reports (without WARNING) whether the name is
 * already registered or the registry has no free slot; the caller decides
 * what to say and, on REGISTER_FULL, whether to evict and retry.  Startup
 * process only.
 */
static RegisterResult
registerAnchor(const char *name, TransactionId xmin)
{
	int			i;
	AnchorRegistryEntry *slot = NULL;

	SpinLockAcquire(&anchorRegistry->lock);
	if (findEntryLocked(name) != NULL)
	{
		SpinLockRelease(&anchorRegistry->lock);
		return REGISTER_DUPLICATE;
	}
	for (i = 0; i < anchorRegistry->capacity; i++)
	{
		if (!anchorRegistry->entries[i].valid)
		{
			slot = &anchorRegistry->entries[i];
			break;
		}
	}
	if (slot == NULL)
	{
		SpinLockRelease(&anchorRegistry->lock);
		return REGISTER_FULL;
	}
	strlcpy(slot->rp_name, name, MAXFNAMELEN);
	slot->xmin = xmin;
	slot->ordinal = anchorRegistry->next_ordinal++;
	slot->valid = true;
	SpinLockRelease(&anchorRegistry->lock);
	return REGISTER_OK;
}

/*
 * Register, evicting the oldest unpublished anchor once when the registry
 * is full (v4.7 R-6).  Called only after the anchor's file is durable, so a
 * failed write never costs an existing anchor.  Startup process only.
 */
static RegisterResult
registerAnchorEvicting(const char *name, TransactionId xmin)
{
	RegisterResult res = registerAnchor(name, xmin);

	if (res == REGISTER_FULL && evictOldestUnpublished(name))
		res = registerAnchor(name, xmin);
	return res;
}

bool
AnchorSnapshotLookup(const char *rp_name, TransactionId *xmin)
{
	AnchorRegistryEntry *e;
	bool		found = false;

	if (!registryEnabled() || rp_name == NULL || rp_name[0] == '\0')
		return false;

	SpinLockAcquire(&anchorRegistry->lock);
	e = findEntryLocked(rp_name);
	if (e != NULL)
	{
		*xmin = e->xmin;
		found = true;
	}
	SpinLockRelease(&anchorRegistry->lock);
	return found;
}


/* ------------------------------------------------------------------
 * Names and files
 * ------------------------------------------------------------------
 */

/*
 * A restore point name becomes a path component and a value in the
 * field:value grammar of the snapshot file; the kernel validates neither
 * at creation.  Reject what would break either.
 */
static bool
anchorNameIsValid(const char *name)
{
	size_t		len;
	const unsigned char *p;

	if (name == NULL)
		return false;
	len = strlen(name);
	if (len == 0 || len >= MAXFNAMELEN)
		return false;
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return false;
	if (len > 4 && strcmp(name + len - 4, ".tmp") == 0)
		return false;
	for (p = (const unsigned char *) name; *p; p++)
	{
		if (*p == '/' || *p == '\\' || *p < 0x20 || *p == 0x7f)
			return false;
	}
	return true;
}

/*
 * A copy of name with control characters replaced, for log lines about a
 * name that failed validation.  Returns a static buffer.
 */
static const char *
printableName(const char *name)
{
	static char buf[MAXFNAMELEN];

	strlcpy(buf, name, MAXFNAMELEN);
	pg_clean_ascii(buf);
	return buf;
}

static void
anchorFilePath(char *path, size_t len, const char *name, bool tmp)
{
	snprintf(path, len, ANCHOR_SNAPSHOT_DIR "/%s%s", name, tmp ? ".tmp" : "");
}

/*
 * write() everything in buf, WARNING on failure.  Returns false on failure.
 */
static bool
writeAll(int fd, const char *path, const char *buf, size_t len)
{
	while (len > 0)
	{
		ssize_t		rc;

		errno = 0;
		rc = write(fd, buf, len);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not write anchor snapshot file \"%s\": %m",
							path)));
			return false;
		}
		if (rc == 0)
		{
			/* write() returning 0 with no error: treat as ENOSPC */
			errno = ENOSPC;
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not write anchor snapshot file \"%s\": %m",
							path)));
			return false;
		}
		buf += rc;
		len -= rc;
	}
	return true;
}

/*
 * Serialise the snapshot into <name>.tmp and durably rename it into place.
 * The format is the exported-snapshot text format restricted to the
 * fields a recovery snapshot carries, framed by the anchor's identity and
 * a checksum:
 *
 *	fmt:1
 *	rp_name:<name>
 *	tli:<timeline>	the restore-point record's timeline ...
 *	lsn:<X/X>		... and end LSN (what gp_create_restore_point returned)
 *	xmin:<xid>
 *	xmax:<xid>
 *	xcnt:0
 *	sof:<0|1>
 *	sxcnt:<n>		(sof:0 only)
 *	sxp:<xid>		(n lines, sof:0 only)
 *	rec:1
 *	crc:<8 hex digits>	CRC-32C of every byte above
 *
 * Every failure is a WARNING; the .tmp file is removed and false returned.
 * The file is streamed through a stack buffer: the startup process has no
 * per-record memory context to palloc in.
 */
static bool
writeAnchorFile(const char *name, TimeLineID tli, XLogRecPtr lsn,
				TransactionId xmin, TransactionId xmax,
				const TransactionId *xids, int nxids, bool suboverflowed)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	char		buf[8192];
	char		crcline[32];
	int			used = 0;
	int			fd;
	int			i;
	int			n;
	pg_crc32c	crc;

	anchorFilePath(path, sizeof(path), name, false);
	anchorFilePath(tmppath, sizeof(tmppath), name, true);

	fd = OpenTransientFile(tmppath, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not create anchor snapshot file \"%s\": %m",
						tmppath)));
		return false;
	}

	INIT_CRC32C(crc);

#define ANCHOR_APPEND(...) \
	do { \
		int _n = snprintf(buf + used, sizeof(buf) - used, __VA_ARGS__); \
		if (_n < 0 || _n >= (int) (sizeof(buf) - used)) \
		{ \
			if (!writeAll(fd, tmppath, buf, used)) \
				goto fail; \
			used = 0; \
			_n = snprintf(buf, sizeof(buf), __VA_ARGS__); \
			if (_n < 0 || _n >= (int) sizeof(buf)) \
			{ \
				/* a single line larger than the buffer: not this grammar */ \
				ereport(WARNING, \
						(errmsg("could not write anchor snapshot file \"%s\": line too long", \
								tmppath))); \
				goto fail; \
			} \
		} \
		COMP_CRC32C(crc, buf + used, _n); \
		used += _n; \
	} while (0)

	ANCHOR_APPEND("fmt:%d\n", ANCHOR_FILE_FORMAT);
	ANCHOR_APPEND("rp_name:%s\n", name);
	ANCHOR_APPEND("tli:%u\n", tli);
	ANCHOR_APPEND("lsn:%X/%X\n", (uint32) (lsn >> 32), (uint32) lsn);
	ANCHOR_APPEND("xmin:%u\n", xmin);
	ANCHOR_APPEND("xmax:%u\n", xmax);
	ANCHOR_APPEND("xcnt:0\n");
	if (suboverflowed)
		ANCHOR_APPEND("sof:1\n");
	else
	{
		ANCHOR_APPEND("sof:0\n");
		ANCHOR_APPEND("sxcnt:%d\n", nxids);
		for (i = 0; i < nxids; i++)
			ANCHOR_APPEND("sxp:%u\n", xids[i]);
	}
	ANCHOR_APPEND("rec:1\n");
#undef ANCHOR_APPEND

	FIN_CRC32C(crc);
	n = snprintf(crcline, sizeof(crcline), "crc:%08X\n", crc);
	if ((used > 0 && !writeAll(fd, tmppath, buf, used)) ||
		!writeAll(fd, tmppath, crcline, n))
		goto fail;
	used = 0;

#ifdef FAULT_INJECTOR
	if (SIMPLE_FAULT_INJECTOR("anchor_snapshot_export_write") == FaultInjectorTypeSkip)
	{
		ereport(WARNING,
				(errmsg("could not write anchor snapshot file \"%s\": fault injected",
						tmppath)));
		goto fail;
	}
#endif

	if (CloseTransientFile(fd) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not close anchor snapshot file \"%s\": %m",
						tmppath)));
		fd = -1;
		goto fail;
	}
	fd = -1;

	/* fsyncs the file, renames, fsyncs the directory */
	if (durable_rename(tmppath, path, WARNING) != 0)
		goto fail;

	return true;

fail:
	if (fd >= 0)
		CloseTransientFile(fd);
	if (unlink(tmppath) != 0 && errno != ENOENT)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove anchor snapshot file \"%s\": %m",
						tmppath)));
	return false;
}

/*
 * One "key:value\n" line of the file grammar.  Advances *cur past the line
 * and returns the value's span; false when the line is missing, has another
 * key, or is unterminated.
 */
static bool
anchorNextField(char **cur, const char *end, const char *key,
				const char **val, size_t *vlen)
{
	size_t		klen = strlen(key);
	char	   *p = *cur;
	char	   *nl;

	if ((size_t) (end - p) < klen + 1 || memcmp(p, key, klen) != 0 ||
		p[klen] != ':')
		return false;
	p += klen + 1;
	nl = memchr(p, '\n', end - p);
	if (nl == NULL)
		return false;
	*val = p;
	*vlen = nl - p;
	*cur = nl + 1;
	return true;
}

/* A decimal or hexadecimal unsigned value filling the whole span. */
static bool
anchorParseUint(const char *val, size_t vlen, int base, unsigned long max,
				unsigned long *out)
{
	char		tmp[32];
	char	   *endp;
	unsigned long v;

	if (vlen == 0 || vlen >= sizeof(tmp))
		return false;
	memcpy(tmp, val, vlen);
	tmp[vlen] = '\0';
	if (!isalnum((unsigned char) tmp[0]))
		return false;
	errno = 0;
	v = strtoul(tmp, &endp, base);
	if (errno != 0 || *endp != '\0' || v > max)
		return false;
	*out = v;
	return true;
}

/*
 * The startup process must never ERROR on our account: an ERROR there is
 * FATAL and would make every start of the standby fail on the same file.
 * Allocate without the out-of-memory ERROR and let the caller WARN.
 */
static void *
startupAlloc(size_t size)
{
	return palloc_extended(size, MCXT_ALLOC_NO_OOM);
}

/*
 * The largest file writeAnchorFile() can produce: a dozen fixed lines of
 * at most 80 bytes, plus one "sxp:<xid>\n" line (at most 16 bytes) per
 * known-assigned xid, of which there are at most GetMaxSnapshotSubxidCount().
 * Anything bigger was not written by this server.
 */
static uint64
anchorFileMaxSize(void)
{
	return 16 * 80 + (uint64) 16 * GetMaxSnapshotSubxidCount();
}

/*
 * Read and validate the anchor's file whole, returning the restore-point
 * record's identity and the xmin.  The grammar is checked strictly: every
 * field in order, the rp_name line naming this anchor, counts matching,
 * xids normal, the CRC over everything before the crc line matching, and
 * nothing after it.  A file that fails any of this is not one this server
 * wrote whole, and registering an anchor from it could pin readers to a
 * snapshot that is not the restore point's.  Startup only; every failure
 * is a WARNING.
 */
static bool
parseAnchorFile(const char *name, TimeLineID *tli, XLogRecPtr *lsn,
				TransactionId *xmin)
{
	char		path[MAXPGPATH];
	struct stat st;
	int			fd;
	char	   *buf = NULL;
	char	   *cur;
	char	   *end;
	size_t		size;
	size_t		got = 0;
	const char *val;
	size_t		vlen;
	unsigned long v;
	unsigned long hi;
	unsigned long lo;
	unsigned long sof;
	unsigned long sxcnt;
	unsigned long i;
	pg_crc32c	crc;
	const char *why = NULL;

	anchorFilePath(path, sizeof(path), name, false);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not open anchor snapshot file \"%s\": %m", path)));
		return false;
	}
	if (fstat(fd, &st) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not stat anchor snapshot file \"%s\": %m", path)));
		CloseTransientFile(fd);
		return false;
	}
	if (st.st_size <= 0 || (uint64) st.st_size > anchorFileMaxSize())
	{
		ereport(WARNING,
				(errmsg("anchor snapshot file \"%s\" is empty or larger than any file this server writes (%lld bytes, limit %llu)",
						path, (long long) st.st_size,
						(unsigned long long) anchorFileMaxSize())));
		CloseTransientFile(fd);
		return false;
	}
	size = (size_t) st.st_size;
	buf = startupAlloc(size + 1);
	if (buf == NULL)
	{
		ereport(WARNING,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not read anchor snapshot file \"%s\": out of memory", path)));
		CloseTransientFile(fd);
		return false;
	}
	while (got < size)
	{
		ssize_t		rc = read(fd, buf + got, size - got);

		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not read anchor snapshot file \"%s\": %m", path)));
			CloseTransientFile(fd);
			pfree(buf);
			return false;
		}
		got += rc;
	}
	CloseTransientFile(fd);
	buf[size] = '\0';

	/* the trailer: crc:XXXXXXXX\n over everything before it */
	if (size < 13 || memcmp(buf + size - 13, "crc:", 4) != 0 ||
		buf[size - 1] != '\n' ||
		!anchorParseUint(buf + size - 9, 8, 16, 0xFFFFFFFFUL, &v))
	{
		why = "missing or malformed crc line";
		goto refuse;
	}
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, buf, size - 13);
	FIN_CRC32C(crc);
	if (crc != (pg_crc32c) v)
	{
		why = "checksum mismatch";
		goto refuse;
	}

	cur = buf;
	end = buf + size - 13;

	if (!anchorNextField(&cur, end, "fmt", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, INT_MAX, &v) || v != ANCHOR_FILE_FORMAT)
	{
		why = "unknown format";
		goto refuse;
	}
	if (!anchorNextField(&cur, end, "rp_name", &val, &vlen) ||
		vlen != strlen(name) || memcmp(val, name, vlen) != 0)
	{
		why = "rp_name line does not name this restore point";
		goto refuse;
	}
	if (!anchorNextField(&cur, end, "tli", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 0xFFFFFFFFUL, &v) || v == 0)
	{
		why = "invalid tli line";
		goto refuse;
	}
	*tli = (TimeLineID) v;
	if (!anchorNextField(&cur, end, "lsn", &val, &vlen) ||
		memchr(val, '/', vlen) == NULL)
	{
		why = "invalid lsn line";
		goto refuse;
	}
	{
		const char *slash = memchr(val, '/', vlen);

		if (!anchorParseUint(val, slash - val, 16, 0xFFFFFFFFUL, &hi) ||
			!anchorParseUint(slash + 1, vlen - (slash - val) - 1, 16,
							 0xFFFFFFFFUL, &lo))
		{
			why = "invalid lsn line";
			goto refuse;
		}
	}
	*lsn = ((XLogRecPtr) hi << 32) | lo;
	if (XLogRecPtrIsInvalid(*lsn))
	{
		why = "invalid lsn line";
		goto refuse;
	}
	if (!anchorNextField(&cur, end, "xmin", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 0xFFFFFFFFUL, &v) ||
		!TransactionIdIsNormal((TransactionId) v))
	{
		why = "invalid xmin line";
		goto refuse;
	}
	*xmin = (TransactionId) v;
	if (!anchorNextField(&cur, end, "xmax", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 0xFFFFFFFFUL, &v) ||
		!TransactionIdIsNormal((TransactionId) v))
	{
		why = "invalid xmax line";
		goto refuse;
	}
	if (!anchorNextField(&cur, end, "xcnt", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, INT_MAX, &v) || v != 0)
	{
		why = "invalid xcnt line";
		goto refuse;
	}
	if (!anchorNextField(&cur, end, "sof", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 1, &sof))
	{
		why = "invalid sof line";
		goto refuse;
	}
	if (sof == 0)
	{
		/* every sxp line is at least "sxp:N\n": bound the count by the bytes left */
		if (!anchorNextField(&cur, end, "sxcnt", &val, &vlen) ||
			!anchorParseUint(val, vlen, 10, INT_MAX, &sxcnt) ||
			sxcnt > (unsigned long) (end - cur) / 6)
		{
			why = "invalid sxcnt line";
			goto refuse;
		}
		for (i = 0; i < sxcnt; i++)
		{
			if (!anchorNextField(&cur, end, "sxp", &val, &vlen) ||
				!anchorParseUint(val, vlen, 10, 0xFFFFFFFFUL, &v) ||
				!TransactionIdIsNormal((TransactionId) v))
			{
				why = "sxp lines do not match sxcnt or carry an invalid xid";
				goto refuse;
			}
		}
	}
	if (!anchorNextField(&cur, end, "rec", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 1, &v) || v != 1)
	{
		why = "invalid rec line";
		goto refuse;
	}
	if (cur != end)
	{
		why = "unexpected data after the rec line";
		goto refuse;
	}

	pfree(buf);
	return true;

refuse:
	ereport(WARNING,
			(errmsg("anchor snapshot file \"%s\" refused: %s", path, why)));
	pfree(buf);
	return false;
}

/*
 * Remove every file in the directory except the one named keep (may be
 * NULL).  Failures are LOG/WARNING; a leftover file is harmless because
 * nothing reads a file that is not registered.
 */
static void
sweepAnchorFiles(const char *keep)
{
	DIR		   *dir;
	struct dirent *de;
	char		path[MAXPGPATH];

	dir = AllocateDir(ANCHOR_SNAPSHOT_DIR);
	if (dir == NULL)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", ANCHOR_SNAPSHOT_DIR)));
		return;
	}
	while ((de = ReadDirExtended(dir, ANCHOR_SNAPSHOT_DIR, LOG)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (keep != NULL && strcmp(de->d_name, keep) == 0)
			continue;
		snprintf(path, sizeof(path), ANCHOR_SNAPSHOT_DIR "/%s", de->d_name);
		(void) durable_unlink(path, WARNING);
	}
	FreeDir(dir);
}


/* ------------------------------------------------------------------
 * Startup process entry points
 * ------------------------------------------------------------------
 */

/*
 * Called once from StartupXLOG at every server start, recovery or not:
 * creates the directory when a data directory initialised by an older
 * binary lacks it, rebuilds the anchor the GUC names when this is a hot
 * standby, and sweeps every other file.  Not repeated on reload.
 *
 * redoStart is where this start's redo begins (invalid outside recovery)
 * and history the recovery timeline history.  The anchor is registered
 * here only when its restore-point record ends at or before redoStart:
 * that WAL is applied on disk.  A record past redoStart will be replayed
 * again, and until it is the data state predates the anchor -- hot standby
 * can open for connections before then -- so the anchor is held pending
 * and AnchorSnapshotExportOnRestorePoint registers it when replay meets
 * the identical record.  A file exported on a timeline the recovery
 * history does not have at that LSN (the node followed a fork) is swept.
 *
 * The sweep spares the file the GUC names whenever this is a standby
 * (archive recovery), even when the registry is off or hot standby is
 * disabled and the file was therefore not examined: a start with the
 * feature switched off must not destroy the published anchor for the
 * start that switches it back on.  A primary keeps nothing.
 */
void
AnchorSnapshotStartup(XLogRecPtr redoStart, List *history)
{
	struct stat st;
	const char *name = whpg_hot_standby_anchor_name;
	bool		keep = false;

	/* remember the boot value for the reload comparison */
	strlcpy(lastSeenAnchorName, name ? name : "", MAXFNAMELEN);
	lastSeenAnchorNameSet = true;
	pendingSet = false;

	if (stat(ANCHOR_SNAPSHOT_DIR, &st) != 0)
	{
		if (MakePGDirectory(ANCHOR_SNAPSHOT_DIR) != 0 && errno != EEXIST)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m",
							ANCHOR_SNAPSHOT_DIR)));
			return;
		}
		/* make the new directory entry durable; WARNING only */
		{
			int			dfd = OpenTransientFile(".", O_RDONLY | PG_BINARY);

			if (dfd >= 0)
			{
				if (pg_fsync(dfd) != 0)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not fsync data directory after creating \"%s\": %m",
									ANCHOR_SNAPSHOT_DIR)));
				CloseTransientFile(dfd);
			}
		}
	}

	if (ArchiveRecoveryRequested && name != NULL && name[0] != '\0' &&
		anchorNameIsValid(name) && !(registryEnabled() && EnableHotStandby))
	{
		/* not examined at this start; kept for the start that will */
		keep = true;
		ereport(LOG,
				(errmsg("anchor snapshot file for restore point \"%s\" kept but not registered: %s",
						name,
						registryEnabled() ? "hot standby is disabled" :
						"anchor snapshots are disabled (whpg_max_anchor_snapshots = 0)")));
	}
	else if (registryEnabled() && ArchiveRecoveryRequested && EnableHotStandby &&
			 name != NULL && name[0] != '\0')
	{
		TimeLineID	tli;
		XLogRecPtr	lsn;
		TransactionId xmin;

		if (!anchorNameIsValid(name))
			ereport(WARNING,
					(errmsg("anchor snapshot for restore point \"%s\" not re-registered: invalid name",
							printableName(name))));
		else if (parseAnchorFile(name, &tli, &lsn, &xmin))
		{
			/*
			 * The record ends at lsn; the byte before it lies inside the
			 * record, so its timeline is the record's even when a fork
			 * begins exactly at the record's end.
			 */
			TimeLineID	histTLI = (history != NIL) ?
			tliOfPointInHistory(lsn - 1, history) : 0;

			if (histTLI != tli)
				ereport(WARNING,
						(errmsg("anchor snapshot for restore point \"%s\" not re-registered: exported on timeline %u, but the recovery history has timeline %u at %X/%X",
								name, tli, histTLI,
								(uint32) (lsn >> 32), (uint32) lsn)));
			else if (XLogRecPtrIsInvalid(redoStart))
				ereport(WARNING,
						(errmsg("anchor snapshot for restore point \"%s\" not re-registered: no redo start point",
								name)));
			else if (lsn <= redoStart)
			{
				if (registerAnchor(name, xmin) == REGISTER_OK)
				{
					keep = true;
					ereport(LOG,
							(errmsg("re-registered anchor snapshot for restore point \"%s\" (xmin %u, record %X/%X before redo start %X/%X)",
									name, xmin,
									(uint32) (lsn >> 32), (uint32) lsn,
									(uint32) (redoStart >> 32), (uint32) redoStart)));
				}
			}
			else
			{
				pendingSet = true;
				strlcpy(pendingName, name, MAXFNAMELEN);
				pendingTLI = tli;
				pendingLSN = lsn;
				pendingXmin = xmin;
				keep = true;
				ereport(LOG,
						(errmsg("anchor snapshot for restore point \"%s\" held pending until replay reaches %X/%X on timeline %u (redo starts at %X/%X)",
								name,
								(uint32) (lsn >> 32), (uint32) lsn, tli,
								(uint32) (redoStart >> 32), (uint32) redoStart)));
			}
		}
		if (!keep)
			ereport(LOG,
					(errmsg("anchor snapshot for restore point \"%s\" not re-registered; anchored queries will fail until the next publication",
							name)));
	}

	sweepAnchorFiles(keep ? name : NULL);
}

/*
 * Called by the startup process right after a restore-point record has been
 * applied (and consistency re-checked), before the replay pause is taken.
 * Exports the node's snapshot at this instant and registers it.
 */
void
AnchorSnapshotExportOnRestorePoint(XLogReaderState *record)
{
	xl_restore_point *rp;
	char		name[MAXFNAMELEN];
	TransactionId xmin;
	TransactionId xmax;
	bool		suboverflowed;
	bool		isRestorePoint;
	int			nxids;

	if (!registryEnabled())
		return;
	/* restore-point records are replayed in crash recovery too */
	if (!ArchiveRecoveryRequested)
		return;

	isRestorePoint = (XLogRecGetRmid(record) == RM_XLOG_ID &&
					  (XLogRecGetInfo(record) & ~XLR_INFO_MASK) == XLOG_RESTORE_POINT);
	if (isRestorePoint)
	{
		rp = (xl_restore_point *) XLogRecGetData(record);
		/* the record's field is fixed-width; do not trust it to be terminated */
		memcpy(name, rp->rp_name, MAXFNAMELEN);
		name[MAXFNAMELEN - 1] = '\0';
	}

	/*
	 * A pending anchor (kept across a restart, not yet confirmed) is
	 * registered from its file when this is its record: the file was
	 * written at this very position by an earlier replay with a complete
	 * standby snapshot and has passed the whole-file check at start, so
	 * re-deriving the snapshot would add nothing and could lose the anchor
	 * (standbyState may still be below READY here after a restart under a
	 * subtransaction-heavy load).  Replay passing its LSN without meeting
	 * the record means the WAL stream differs from the one it was exported
	 * from, and the file goes.
	 */
	if (pendingSet)
	{
		bool		match = isRestorePoint &&
		strcmp(name, pendingName) == 0 &&
		ThisTimeLineID == pendingTLI &&
		record->EndRecPtr == pendingLSN;

		if (match)
		{
			RegisterResult res;

			pendingSet = false;
			res = registerAnchorEvicting(pendingName, pendingXmin);
			if (res == REGISTER_DUPLICATE)
				ereport(WARNING,
						(errmsg("pending anchor snapshot for restore point \"%s\" not registered: the name is already registered",
								pendingName)));
			else if (res == REGISTER_FULL)
				ereport(WARNING,
						(errmsg("pending anchor snapshot for restore point \"%s\" not registered: registry full (whpg_max_anchor_snapshots = %d) and only the published anchor is registered",
								pendingName, anchorRegistry->capacity)));
			else
			{
				ereport(LOG,
						(errmsg("replay reached the restore-point record of pending anchor snapshot \"%s\" at %X/%X; registered from its file (xmin %u)",
								pendingName,
								(uint32) (pendingLSN >> 32), (uint32) pendingLSN,
								pendingXmin)));
				if (whpg_hot_standby_anchor_name != NULL &&
					strcmp(pendingName, whpg_hot_standby_anchor_name) == 0)
					(void) applyPublication(pendingName);
			}
			/* the record itself is now a duplicate of a registered name */
			return;
		}
		else if (record->EndRecPtr > pendingLSN)
		{
			char		path[MAXPGPATH];

			pendingSet = false;
			anchorFilePath(path, sizeof(path), pendingName, false);
			(void) durable_unlink(path, WARNING);
			ereport(WARNING,
					(errmsg("pending anchor snapshot for restore point \"%s\" dropped: replay passed %X/%X without meeting its restore-point record",
							pendingName,
							(uint32) (pendingLSN >> 32), (uint32) pendingLSN)));
		}
	}

	if (!isRestorePoint)
		return;

	/*
	 * KnownAssignedXids is complete only once the running-xacts snapshot
	 * has been applied; an earlier snapshot would declare in-progress
	 * primary transactions visible.  This node then has no anchor for this
	 * restore point.
	 */
	if (standbyState != STANDBY_SNAPSHOT_READY)
	{
		ereport(EnableHotStandby ? LOG : DEBUG1,
				(errmsg("anchor snapshot for restore point \"%s\" not exported: standby snapshot not ready",
						name)));
		return;
	}

	if (!anchorNameIsValid(name))
	{
		ereport(WARNING,
				(errmsg("anchor snapshot for restore point \"%s\" not exported: invalid name",
						printableName(name)),
				 errdetail("Restore point names used as anchors must be non-empty, contain no path separators or control characters, and not end in \".tmp\".")));
		return;
	}

	{
		TransactionId dummy;

		if (AnchorSnapshotLookup(name, &dummy))
		{
			ereport(WARNING,
					(errmsg("anchor snapshot for restore point \"%s\" not exported: name already registered",
							name),
					 errdetail("The existing anchor is kept; a restore point name can become an anchor only once.")));
			return;
		}
	}

	/*
	 * A pending anchor counts as registered too.  Restore point names are
	 * not unique on the primary, and retirement frees the name, so after a
	 * crash redo can meet an EARLIER restore point of the very name whose
	 * later snapshot file is held pending (its record lies ahead).  Exporting
	 * here would overwrite that file with this record's snapshot and, when
	 * the name is the published one, complete the publication with the
	 * wrong cut.  The pending file is the one the GUC meant; this record's
	 * instance never becomes an anchor.
	 */
	if (pendingSet && strcmp(name, pendingName) == 0)
	{
		ereport(WARNING,
				(errmsg("anchor snapshot for restore point \"%s\" not exported: a snapshot file of that name from an earlier run is pending registration at %X/%X",
						name,
						(uint32) (pendingLSN >> 32), (uint32) pendingLSN),
				 errdetail("The pending file is kept; the record just replayed is an earlier restore point of the same name.")));
		return;
	}

	if (exportXids == NULL)
	{
		exportXids = (TransactionId *)
			malloc(GetMaxSnapshotSubxidCount() * sizeof(TransactionId));
		if (exportXids == NULL)
		{
			ereport(WARNING,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("anchor snapshot for restore point \"%s\" not exported: out of memory",
							name)));
			return;
		}
	}

	nxids = GetKnownAssignedXidsSnapshot(exportXids, &xmin, &xmax, &suboverflowed);

	if (!writeAnchorFile(name, ThisTimeLineID, record->EndRecPtr,
						 xmin, xmax, exportXids, nxids, suboverflowed))
		return;

	/*
	 * The file is durable; now take a slot.  A full registry evicts the
	 * oldest unpublished anchor (entry and file) only at this point, so a
	 * write that failed above has cost nothing.  A duplicate was ruled out
	 * above (single writer), so the only failure left is a registry holding
	 * nothing but the published anchor: undo the file, keep the registry as
	 * it was.
	 */
	if (registerAnchorEvicting(name, xmin) != REGISTER_OK)
	{
		char		path[MAXPGPATH];

		anchorFilePath(path, sizeof(path), name, false);
		(void) durable_unlink(path, WARNING);
		ereport(WARNING,
				(errmsg("anchor snapshot for restore point \"%s\" not exported: registry full (whpg_max_anchor_snapshots = %d) and only the published anchor is registered",
						name, anchorRegistry->capacity),
				 errhint("Raise whpg_max_anchor_snapshots.")));
		return;
	}

	ereport(LOG,
			(errmsg("exported anchor snapshot for restore point \"%s\" (xmin %u, xmax %u, %d known xids%s, record %X/%X)",
					name, xmin, xmax, nxids,
					suboverflowed ? ", overflowed" : "",
					(uint32) (record->EndRecPtr >> 32), (uint32) record->EndRecPtr)));

	/*
	 * The GUC may already name this anchor: a reload that arrived before
	 * the record, or a pending anchor registered just now.  The export
	 * then completes the publication.
	 */
	if (whpg_hot_standby_anchor_name != NULL &&
		strcmp(name, whpg_hot_standby_anchor_name) == 0)
		(void) applyPublication(name);

	SIMPLE_FAULT_INJECTOR("anchor_snapshot_exported");
}

/*
 * Make room in a full registry for newname: invalidate the anchor with the
 * smallest ordinal that is not the published one.  Only the anchor the GUC
 * names is ever imported, so an unpublished entry has no reader and giving
 * up its slot destroys nothing anyone can read.  Returns false when every
 * entry is the published anchor.  Startup process only.
 */
static bool
evictOldestUnpublished(const char *newname)
{
	const char *published = whpg_hot_standby_anchor_name ? whpg_hot_standby_anchor_name : "";
	AnchorRegistryEntry *victim = NULL;
	char		victimName[MAXFNAMELEN];
	char		path[MAXPGPATH];
	int			i;

	SpinLockAcquire(&anchorRegistry->lock);
	for (i = 0; i < anchorRegistry->capacity; i++)
	{
		AnchorRegistryEntry *e = &anchorRegistry->entries[i];

		if (!e->valid || strcmp(e->rp_name, published) == 0)
			continue;
		if (victim == NULL || e->ordinal < victim->ordinal)
			victim = e;
	}
	if (victim != NULL)
	{
		strlcpy(victimName, victim->rp_name, MAXFNAMELEN);
		victim->valid = false;
	}
	SpinLockRelease(&anchorRegistry->lock);

	if (victim == NULL)
		return false;

	anchorFilePath(path, sizeof(path), victimName, false);
	(void) durable_unlink(path, WARNING);
	ereport(LOG,
			(errmsg("evicted anchor snapshot for restore point \"%s\" to make room for \"%s\": registry full (whpg_max_anchor_snapshots = %d)",
					victimName, newname, anchorRegistry->capacity)));
	return true;
}

/*
 * Delete an anchor: entry and file.  The file is removed even when no
 * entry exists (a crash between the rename and the registration leaves
 * exactly that).  Startup process only.
 */
void
AnchorSnapshotInvalidate(const char *rp_name)
{
	AnchorRegistryEntry *e = NULL;
	char		path[MAXPGPATH];

	if (anchorRegistry == NULL || rp_name == NULL || !anchorNameIsValid(rp_name))
		return;

	if (anchorRegistry->capacity > 0)
	{
		SpinLockAcquire(&anchorRegistry->lock);
		e = findEntryLocked(rp_name);
		if (e != NULL)
			e->valid = false;
		SpinLockRelease(&anchorRegistry->lock);
	}

	anchorFilePath(path, sizeof(path), rp_name, false);
	if (unlink(path) != 0 && errno != ENOENT)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove anchor snapshot file \"%s\": %m", path)));
	else if (e != NULL)
		ereport(LOG,
				(errmsg("invalidated anchor snapshot for restore point \"%s\"", rp_name)));
}

/*
 * Publication of name: retire every anchor registered before it (entry and
 * file).  Returns false, retiring nothing, when name is not registered --
 * nothing is deleted on the strength of a name the registry cannot vouch
 * for.  Called on the reload that changes the GUC and again by the export
 * of the name the GUC already carries.  Startup process only.
 */
static bool
applyPublication(const char *name)
{
	AnchorRegistryEntry *e;
	uint64		ordinal;
	int			nretire = 0;
	int			i;

	if (!registryEnabled() || name == NULL || name[0] == '\0' ||
		!anchorNameIsValid(name))
		return false;

	/* the names are collected under the spinlock, the files unlinked outside it */
	if (retireNames == NULL)
	{
		retireNames = (char *) malloc(anchorRegistry->capacity * MAXFNAMELEN);
		if (retireNames == NULL)
		{
			ereport(WARNING,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("publication of anchor \"%s\" retired nothing: out of memory",
							name)));
			return false;
		}
	}

	SpinLockAcquire(&anchorRegistry->lock);
	e = findEntryLocked(name);
	if (e != NULL)
	{
		ordinal = e->ordinal;
		for (i = 0; i < anchorRegistry->capacity; i++)
		{
			AnchorRegistryEntry *o = &anchorRegistry->entries[i];

			if (o->valid && o->ordinal < ordinal)
			{
				o->valid = false;
				strlcpy(retireNames + nretire * MAXFNAMELEN, o->rp_name, MAXFNAMELEN);
				nretire++;
			}
		}
	}
	SpinLockRelease(&anchorRegistry->lock);

	if (e == NULL)
		return false;

	for (i = 0; i < nretire; i++)
	{
		char		path[MAXPGPATH];

		anchorFilePath(path, sizeof(path), retireNames + i * MAXFNAMELEN, false);
		(void) durable_unlink(path, WARNING);
		ereport(LOG,
				(errmsg("retired anchor snapshot for restore point \"%s\": superseded by \"%s\"",
						retireNames + i * MAXFNAMELEN, name)));
	}
	return true;
}

/*
 * Called by the startup process after it has re-read the configuration.
 * A newly named, registered anchor retires every anchor registered before
 * it; an empty or unknown name retires nothing.  A name not registered yet
 * is not lost: its export completes the publication when the record
 * arrives (AnchorSnapshotExportOnRestorePoint).
 */
void
AnchorSnapshotOnConfigReload(void)
{
	const char *name = whpg_hot_standby_anchor_name ? whpg_hot_standby_anchor_name : "";

	if (!lastSeenAnchorNameSet || strcmp(name, lastSeenAnchorName) == 0)
		return;
	strlcpy(lastSeenAnchorName, name, MAXFNAMELEN);

	if (!registryEnabled() || name[0] == '\0' || !anchorNameIsValid(name))
		return;

	if (!applyPublication(name))
		ereport(LOG,
				(errmsg("anchor \"%s\" named by whpg_hot_standby_anchor_name is not registered on this node; no anchor retired",
						name),
				 errdetail("The publication completes when the restore point of that name is exported.")));
}

/*
 * Called by the startup process at the end of recovery, before the shared
 * recovery state flips to done: nothing may outlive recovery.
 */
void
AnchorSnapshotClearAll(void)
{
	int			i;
	int			cleared = 0;

	pendingSet = false;

	if (anchorRegistry == NULL)
		return;

	if (anchorRegistry->capacity > 0)
	{
		SpinLockAcquire(&anchorRegistry->lock);
		for (i = 0; i < anchorRegistry->capacity; i++)
		{
			if (anchorRegistry->entries[i].valid)
			{
				anchorRegistry->entries[i].valid = false;
				cleared++;
			}
		}
		SpinLockRelease(&anchorRegistry->lock);
	}

	sweepAnchorFiles(NULL);

	if (cleared > 0)
		ereport(LOG,
				(errmsg("cleared %d anchor snapshot(s) at the end of recovery", cleared)));
}


/* ------------------------------------------------------------------
 * Introspection
 * ------------------------------------------------------------------
 */

static int
anchorInfoCompare(const void *a, const void *b)
{
	const AnchorRegistryEntry *ea = (const AnchorRegistryEntry *) a;
	const AnchorRegistryEntry *eb = (const AnchorRegistryEntry *) b;

	if (ea->ordinal < eb->ordinal)
		return -1;
	if (ea->ordinal > eb->ordinal)
		return 1;
	return 0;
}

/*
 * Copy the registered anchors, oldest registration first, into dst (at most
 * max entries) and return the number registered.  Backed by a short scan
 * under the spinlock; the sort happens on the private copy.
 */
int
AnchorSnapshotList(AnchorSnapshotInfo *dst, int max)
{
	AnchorRegistryEntry *copy;
	int			n = 0;
	int			i;

	if (!registryEnabled() || max <= 0)
		return 0;

	copy = (AnchorRegistryEntry *)
		palloc(sizeof(AnchorRegistryEntry) * anchorRegistry->capacity);

	SpinLockAcquire(&anchorRegistry->lock);
	for (i = 0; i < anchorRegistry->capacity; i++)
	{
		if (anchorRegistry->entries[i].valid)
			copy[n++] = anchorRegistry->entries[i];
	}
	SpinLockRelease(&anchorRegistry->lock);

	if (n > 1)
		qsort(copy, n, sizeof(AnchorRegistryEntry), anchorInfoCompare);

	for (i = 0; i < n && i < max; i++)
	{
		strlcpy(dst[i].rp_name, copy[i].rp_name, MAXFNAMELEN);
		dst[i].xmin = copy[i].xmin;
	}
	pfree(copy);
	return n;
}
