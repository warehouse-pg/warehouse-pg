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
#include "access/parallel.h"
#include "access/timeline.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "cdb/cdbdtxcontextinfo.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "common/string.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/faultinjector.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/sharedsnapshot.h"
#include "utils/snapmgr.h"

/* GUC variables; the entries live in guc_gp.c */
int			whpg_max_anchor_snapshots = 64;
char	   *whpg_hot_standby_anchor_name = NULL;
int			whpg_hot_standby_snapshot_mode = WHPG_SNAPSHOT_MODE_UNANCHORED;

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
#define ANCHOR_FILE_FORMAT	2

static bool anchorNameIsValid(const char *name);
static void anchorFilePath(char *path, size_t len, const char *name, bool tmp);
static bool writeAnchorFile(const char *name, TimeLineID tli, XLogRecPtr lsn,
							TransactionId xmin, TransactionId xmax,
							const TransactionId *xids, int nxids,
							bool suboverflowed);
/*
 * The xid set of a snapshot file, for the installer.  subxip holds subxcnt
 * xids allocated in the caller's memory context (NULL when the list is
 * empty).  An overflowed snapshot carries its list like any other; the
 * flag makes the reader map subtransactions to their parents first.
 */
typedef struct AnchorFileSnapshot
{
	TransactionId xmin;
	TransactionId xmax;
	bool		suboverflowed;
	int			subxcnt;
	TransactionId *subxip;
} AnchorFileSnapshot;

static bool parseAnchorFile(const char *name, TimeLineID *tli,
							XLogRecPtr *lsn, TransactionId *xmin,
							AnchorFileSnapshot *out, StringInfo why);
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
AnchorSnapshotLookup(const char *rp_name, TransactionId *xmin, uint64 *ordinal)
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
		if (ordinal != NULL)
			*ordinal = e->ordinal;
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
 *	fmt:2
 *	rp_name:<name>
 *	tli:<timeline>	the restore-point record's timeline ...
 *	lsn:<X/X>		... and end LSN (what gp_create_restore_point returned)
 *	xmin:<xid>
 *	xmax:<xid>
 *	xcnt:0
 *	sof:<0|1>
 *	sxcnt:<n>
 *	sxp:<xid>		(n lines)
 *	rec:1
 *	crc:<8 hex digits>	CRC-32C of every byte above
 *
 * Unlike an ordinary exported snapshot, the xid list is written whether or
 * not the snapshot overflowed: a recovery snapshot keeps every known xid,
 * top-level ones included, in that list (xip is empty), and the overflow
 * flag only tells the reader to map a subtransaction to its parent through
 * pg_subtrans before searching it.  Format 1 dropped the list on overflow
 * and left an importer with an empty in-progress set.
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
	ANCHOR_APPEND("sof:%d\n", suboverflowed ? 1 : 0);
	ANCHOR_APPEND("sxcnt:%d\n", nxids);
	for (i = 0; i < nxids; i++)
		ANCHOR_APPEND("sxp:%u\n", xids[i]);
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
 * record's identity and the xmin, and, when out is given, the xid set
 * (its subxip allocated in the current memory context).  The grammar is
 * checked strictly: every field in order, the rp_name line naming this
 * anchor, counts matching, xids normal, the CRC over everything before the
 * crc line matching, and nothing after it.  A file that fails any of this
 * is not one this server wrote whole, and registering an anchor from it
 * could pin readers to a snapshot that is not the restore point's.
 *
 * Every failure is reported once: as a WARNING when why is NULL (the
 * startup process, where an ERROR would be FATAL), otherwise appended to
 * why for the caller to raise.  Allocations never ERROR on OOM either way.
 */
static bool
parseAnchorFile(const char *name, TimeLineID *tli, XLogRecPtr *lsn,
				TransactionId *xmin, AnchorFileSnapshot *out, StringInfo why)
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
	unsigned long sxcnt = 0;
	unsigned long i;
	pg_crc32c	crc;
	TransactionId xmax = InvalidTransactionId;
	TransactionId *xids = NULL;
	char		reason[MAXPGPATH + 256];
	enum { FAIL_PLAIN, FAIL_FILE, FAIL_OOM } failkind = FAIL_PLAIN;
	int			save_errno = 0;

	anchorFilePath(path, sizeof(path), name, false);
	/* O_NONBLOCK: a FIFO left in the directory must not hang the opener */
	fd = OpenTransientFile(path, O_RDONLY | O_NONBLOCK | PG_BINARY);
	if (fd < 0)
	{
		failkind = FAIL_FILE;
		save_errno = errno;
		snprintf(reason, sizeof(reason),
				 "could not open anchor snapshot file \"%s\": %s",
				 path, strerror(save_errno));
		goto fail;
	}
	if (fstat(fd, &st) != 0)
	{
		failkind = FAIL_FILE;
		save_errno = errno;
		snprintf(reason, sizeof(reason),
				 "could not stat anchor snapshot file \"%s\": %s",
				 path, strerror(save_errno));
		CloseTransientFile(fd);
		goto fail;
	}
	if (!S_ISREG(st.st_mode))
	{
		snprintf(reason, sizeof(reason),
				 "anchor snapshot file \"%s\" refused: not a regular file", path);
		CloseTransientFile(fd);
		goto fail;
	}
	if (st.st_size <= 0 || (uint64) st.st_size > anchorFileMaxSize())
	{
		snprintf(reason, sizeof(reason),
				 "anchor snapshot file \"%s\" is empty or larger than any file this server writes (%lld bytes, limit %llu)",
				 path, (long long) st.st_size,
				 (unsigned long long) anchorFileMaxSize());
		CloseTransientFile(fd);
		goto fail;
	}
	size = (size_t) st.st_size;
	buf = startupAlloc(size + 1);
	if (buf == NULL)
	{
		failkind = FAIL_OOM;
		snprintf(reason, sizeof(reason),
				 "could not read anchor snapshot file \"%s\": out of memory", path);
		CloseTransientFile(fd);
		goto fail;
	}
	while (got < size)
	{
		ssize_t		rc = read(fd, buf + got, size - got);

		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0)
		{
			if (rc < 0)
			{
				failkind = FAIL_FILE;
				save_errno = errno;
			}
			snprintf(reason, sizeof(reason),
					 "could not read anchor snapshot file \"%s\": %s",
					 path, rc == 0 ? "unexpected end of file" : strerror(save_errno));
			CloseTransientFile(fd);
			goto fail;
		}
		got += rc;
	}
	CloseTransientFile(fd);
	buf[size] = '\0';

	/* the trailer: crc:XXXXXXXX\n over everything before it */
	if (size < 13 || memcmp(buf + size - 13, "crc:", 4) != 0 ||
		buf[size - 1] != '\n' ||
		!anchorParseUint(buf + size - 9, 8, 16, 0xFFFFFFFFUL, &v))
		goto refuse_crc;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, buf, size - 13);
	FIN_CRC32C(crc);
	if (crc != (pg_crc32c) v)
	{
		snprintf(reason, sizeof(reason),
				 "anchor snapshot file \"%s\" refused: checksum mismatch", path);
		goto fail;
	}

	cur = buf;
	end = buf + size - 13;

	if (!anchorNextField(&cur, end, "fmt", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, INT_MAX, &v) || v != ANCHOR_FILE_FORMAT)
		goto refuse_fmt;
	if (!anchorNextField(&cur, end, "rp_name", &val, &vlen) ||
		vlen != strlen(name) || memcmp(val, name, vlen) != 0)
		goto refuse_name;
	if (!anchorNextField(&cur, end, "tli", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 0xFFFFFFFFUL, &v) || v == 0)
		goto refuse_tli;
	*tli = (TimeLineID) v;
	if (!anchorNextField(&cur, end, "lsn", &val, &vlen) ||
		memchr(val, '/', vlen) == NULL)
		goto refuse_lsn;
	{
		const char *slash = memchr(val, '/', vlen);

		if (!anchorParseUint(val, slash - val, 16, 0xFFFFFFFFUL, &hi) ||
			!anchorParseUint(slash + 1, vlen - (slash - val) - 1, 16,
							 0xFFFFFFFFUL, &lo))
			goto refuse_lsn;
	}
	*lsn = ((XLogRecPtr) hi << 32) | lo;
	if (XLogRecPtrIsInvalid(*lsn))
		goto refuse_lsn;
	if (!anchorNextField(&cur, end, "xmin", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 0xFFFFFFFFUL, &v) ||
		!TransactionIdIsNormal((TransactionId) v))
		goto refuse_xmin;
	*xmin = (TransactionId) v;
	if (!anchorNextField(&cur, end, "xmax", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 0xFFFFFFFFUL, &v) ||
		!TransactionIdIsNormal((TransactionId) v))
		goto refuse_xmax;
	xmax = (TransactionId) v;
	if (!anchorNextField(&cur, end, "xcnt", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, INT_MAX, &v) || v != 0)
		goto refuse_xcnt;
	if (!anchorNextField(&cur, end, "sof", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 1, &sof))
		goto refuse_sof;
	/* every sxp line is at least "sxp:N\n": bound the count by the bytes left */
	if (!anchorNextField(&cur, end, "sxcnt", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, INT_MAX, &sxcnt) ||
		sxcnt > (unsigned long) (end - cur) / 6)
		goto refuse_sxcnt;
	if (out != NULL && sxcnt > 0)
	{
		xids = (TransactionId *) startupAlloc(sizeof(TransactionId) * sxcnt);
		if (xids == NULL)
		{
			failkind = FAIL_OOM;
			snprintf(reason, sizeof(reason),
					 "could not read anchor snapshot file \"%s\": out of memory", path);
			goto fail;
		}
	}
	for (i = 0; i < sxcnt; i++)
	{
		if (!anchorNextField(&cur, end, "sxp", &val, &vlen) ||
			!anchorParseUint(val, vlen, 10, 0xFFFFFFFFUL, &v) ||
			!TransactionIdIsNormal((TransactionId) v))
			goto refuse_sxp;
		if (xids != NULL)
			xids[i] = (TransactionId) v;
	}
	if (!anchorNextField(&cur, end, "rec", &val, &vlen) ||
		!anchorParseUint(val, vlen, 10, 1, &v) || v != 1)
		goto refuse_rec;
	if (cur != end)
		goto refuse_tail;

	if (out != NULL)
	{
		out->xmin = *xmin;
		out->xmax = xmax;
		out->suboverflowed = (sof != 0);
		out->subxcnt = (int) sxcnt;
		out->subxip = xids;
	}
	pfree(buf);
	return true;

	/* grammar refusals share one message shape */
refuse_crc:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "missing or malformed crc line");
	goto fail;
refuse_fmt:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "unknown format");
	goto fail;
refuse_name:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "rp_name line does not name this restore point");
	goto fail;
refuse_tli:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "invalid tli line");
	goto fail;
refuse_lsn:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "invalid lsn line");
	goto fail;
refuse_xmin:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "invalid xmin line");
	goto fail;
refuse_xmax:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "invalid xmax line");
	goto fail;
refuse_xcnt:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "invalid xcnt line");
	goto fail;
refuse_sof:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "invalid sof line");
	goto fail;
refuse_sxcnt:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "invalid sxcnt line");
	goto fail;
refuse_sxp:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "sxp lines do not match sxcnt or carry an invalid xid");
	goto fail;
refuse_rec:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "invalid rec line");
	goto fail;
refuse_tail:
	snprintf(reason, sizeof(reason), "anchor snapshot file \"%s\" refused: %s", path, "unexpected data after the rec line");
	goto fail;

fail:
	if (buf != NULL)
		pfree(buf);
	if (xids != NULL)
		pfree(xids);
	if (why != NULL)
		appendStringInfoString(why, reason);
	else if (failkind == FAIL_FILE)
	{
		errno = save_errno;
		ereport(WARNING, (errcode_for_file_access(), errmsg("%s", reason)));
	}
	else if (failkind == FAIL_OOM)
		ereport(WARNING, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("%s", reason)));
	else
		ereport(WARNING, (errmsg("%s", reason)));
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
/*
 * May an overflowed anchor be registered again by this start?  Its readers
 * map subtransactions to their parents through pg_subtrans, and
 * StartupSUBTRANS zeroes every page from the start checkpoint's oldest
 * active xid on; the assignment records that filled those pages lie before
 * the redo start point and are never replayed again.  An anchor whose xid
 * range ends at or before that xid consults only pages the zeroing spared.
 */
static bool
anchorSurvivesStart(bool suboverflowed, TransactionId xmax,
					TransactionId oldestActiveXid)
{
	if (!suboverflowed || !TransactionIdIsValid(oldestActiveXid))
		return true;
	return !TransactionIdFollows(xmax, oldestActiveXid);
}

void
AnchorSnapshotStartup(XLogRecPtr redoStart, List *history,
					  TransactionId oldestActiveXid)
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
		AnchorFileSnapshot fs;

		if (!anchorNameIsValid(name))
			ereport(WARNING,
					(errmsg("anchor snapshot for restore point \"%s\" not re-registered: invalid name",
							printableName(name))));
		else if (parseAnchorFile(name, &tli, &lsn, &xmin, &fs, NULL))
		{
			/*
			 * The record ends at lsn; the byte before it lies inside the
			 * record, so its timeline is the record's even when a fork
			 * begins exactly at the record's end.
			 */
			TimeLineID	histTLI = (history != NIL) ?
			tliOfPointInHistory(lsn - 1, history) : 0;

			if (fs.subxip != NULL)
				pfree(fs.subxip);

			if (!anchorSurvivesStart(fs.suboverflowed, fs.xmax, oldestActiveXid))
				ereport(WARNING,
						(errmsg("anchor snapshot for restore point \"%s\" not re-registered: its overflowed transactions reach past the start checkpoint's oldest active transaction %u (xmax %u)",
								name, oldestActiveXid, fs.xmax),
						 errdetail("This start zeroed the pg_subtrans pages the anchor's readers would map subtransactions through.")));
			else if (histTLI != tli)
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

		if (AnchorSnapshotLookup(name, &dummy, NULL))
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

	/*
	 * Durable: a crash after a plain unlink could bring the file back and
	 * let the next start re-register an anchor whose tuples a cleanup
	 * record the restart never replays again has already removed.
	 */
	anchorFilePath(path, sizeof(path), rp_name, false);
	if (durable_unlink(path, WARNING) != 0 && errno != ENOENT)
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
 * Backend import
 *
 * The installer runs in the backends of a hot standby, from the
 * per-statement snapshot funnel (snapmgr.c): in dispatch-role sessions it
 * follows the mode GUC and the published name, in executors it follows the
 * anchor the dispatcher announced in the transaction context.  It never
 * builds a snapshot itself: GetSnapshotData() has just filled the
 * SnapshotData it is given, so every field the rest of the system relies
 * on (the distributed snapshot for the dispatcher, curcid, the
 * old-snapshot bookkeeping, the xid arrays) is in place; the installer
 * lays the anchor's xid set over the local half and lowers the session's
 * xmin to the anchor's.
 * ------------------------------------------------------------------
 */

/*
 * The anchor this backend last read from disk, keyed by the registration
 * (name and ordinal): a name registered again after retirement is a new
 * anchor even when its xmin happens to be the same.
 */
static struct
{
	bool		valid;
	char		name[MAXFNAMELEN];
	uint64		ordinal;
	AnchorFileSnapshot snap;	/* subxip lives in TopMemoryContext */
}			cachedAnchor;

/*
 * The anchor a REPEATABLE READ transaction's first snapshot installed.
 * Later statements of the transaction re-check it instead of following
 * the GUC; cleared at end of transaction.
 */
static struct
{
	bool		pinned;
	char		name[MAXFNAMELEN];
	TransactionId xmin;
	uint64		ordinal;
}			pinnedAnchor;

/*
 * Does an anchored read apply to this session right now?  The order of
 * the tests keeps the common cases cheap: the GUC and the role first, the
 * recovery state (a shared-memory read until recovery ends, a cached
 * false afterwards) last.  Sessions still initializing take the ordinary
 * snapshot InitPostgres needs, so that a connection can always be made
 * and switch to unanchored mode.
 */
static bool
anchoredReadApplies(void)
{
	if (whpg_hot_standby_snapshot_mode != WHPG_SNAPSHOT_MODE_ANCHORED)
		return false;
	if (Gp_role != GP_ROLE_DISPATCH)
		return false;
	if (IsBackgroundWorker || IsParallelWorker() || !IsNormalProcessingMode())
		return false;
	if (!registryEnabled())
		return false;
	return RecoveryInProgress();
}

bool
AnchorSnapshotSessionAnchored(void)
{
	return anchoredReadApplies();
}

static void
anchorNotRegisteredError(const char *name)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("anchor snapshot \"%s\" is not registered on this node",
					printableName(name)),
			 errdetail("The restore point of that name has not been exported here, "
					   "is pending replay confirmation after a restart, or the "
					   "anchor has been retired, evicted or invalidated."),
			 errhint("Wait for the next publication, or SET whpg_hot_standby_snapshot_mode = unanchored for this session.")));
}

static void
anchorPinnedGoneError(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("anchor snapshot \"%s\" pinned by this transaction is no longer registered",
					printableName(pinnedAnchor.name)),
			 errdetail("A later publication retired the anchor, it was evicted or invalidated, or recovery has ended on this node."),
			 errhint("Roll back and start a new transaction.")));
}

/*
 * Make cachedAnchor hold the file of (name, xmin), reading it unless the
 * cache already does.  The file is read outside every lock; a file that
 * is missing, damaged or disagrees with the registry is a refusal.
 */
static void
loadAnchorIntoCache(const char *name, TransactionId xmin, uint64 ordinal)
{
	TimeLineID	tli;
	XLogRecPtr	lsn;
	TransactionId fxmin;
	AnchorFileSnapshot fs;
	StringInfoData why;
	MemoryContext oldcxt;
	bool		ok;

	if (cachedAnchor.valid && cachedAnchor.ordinal == ordinal &&
		strcmp(cachedAnchor.name, name) == 0)
	{
		Assert(cachedAnchor.snap.xmin == xmin);
		return;
	}

	cachedAnchor.valid = false;
	if (cachedAnchor.snap.subxip != NULL)
	{
		pfree(cachedAnchor.snap.subxip);
		cachedAnchor.snap.subxip = NULL;
	}

	initStringInfo(&why);
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	ok = parseAnchorFile(name, &tli, &lsn, &fxmin, &fs, &why);
	MemoryContextSwitchTo(oldcxt);
	if (!ok)
	{
		char	   *reason = pstrdup(why.data);

		pfree(why.data);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("anchor snapshot \"%s\" could not be read: %s",
						printableName(name), reason)));
	}
	pfree(why.data);
	if (fxmin != xmin)
	{
		if (fs.subxip != NULL)
			pfree(fs.subxip);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("anchor snapshot \"%s\" could not be read: %s",
						printableName(name),
						"the file's xmin does not match the registry")));
	}

	strlcpy(cachedAnchor.name, name, MAXFNAMELEN);
	cachedAnchor.ordinal = ordinal;
	cachedAnchor.snap = fs;
	cachedAnchor.valid = true;
}

static inline void
lowerXmin(TransactionId *target, TransactionId xmin)
{
	if (!TransactionIdIsValid(*target) || TransactionIdPrecedes(xmin, *target))
		*target = xmin;
}

/*
 * The part of an installation common to the dispatcher and the executors:
 * the registered anchor (name, xmin, ordinal) is read into the cache, its
 * xmin published under ProcArrayLock after re-checking the registration,
 * and its xid set laid over the snapshot's local half.
 */
static void
installRegisteredAnchor(Snapshot snapshot, const char *name,
						TransactionId regXmin, uint64 ordinal)
{
	TransactionId recheck;
	uint64		recheckOrdinal;

	loadAnchorIntoCache(name, regXmin, ordinal);

	if (cachedAnchor.snap.subxcnt > GetMaxSnapshotSubxidCount())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("anchor snapshot \"%s\" holds more transaction ids than this server can install",
						printableName(name)),
				 errdetail("The anchor was exported with %d known transaction ids; this server holds at most %d.",
						   cachedAnchor.snap.subxcnt, GetMaxSnapshotSubxidCount())));

	/*
	 * Publish the anchor's xmin before the snapshot is used.  The registry
	 * is re-checked under ProcArrayLock so that an invalidation ordered
	 * "delete the entry, then take ProcArrayLock exclusively, then collect
	 * conflicting readers" either sees this session's xmin or made this
	 * check fail; there is no order in which the session reads with an
	 * xmin nobody knows about.
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	if (!AnchorSnapshotLookup(name, &recheck, &recheckOrdinal) ||
		recheckOrdinal != ordinal)
	{
		LWLockRelease(ProcArrayLock);
		if (pinnedAnchor.pinned)
			anchorPinnedGoneError();
		anchorNotRegisteredError(name);
	}
	lowerXmin(&MyPgXact->xmin, regXmin);
	LWLockRelease(ProcArrayLock);

	lowerXmin(&TransactionXmin, regXmin);
	lowerXmin(&RecentXmin, regXmin);
	lowerXmin(&RecentGlobalXmin, regXmin);
	lowerXmin(&RecentGlobalDataXmin, regXmin);

	/* the local half of the snapshot is the anchor's */
	snapshot->xmin = cachedAnchor.snap.xmin;
	snapshot->xmax = cachedAnchor.snap.xmax;
	snapshot->xcnt = 0;
	snapshot->subxcnt = cachedAnchor.snap.subxcnt;
	if (snapshot->subxcnt > 0)
		memcpy(snapshot->subxip, cachedAnchor.snap.subxip,
			   sizeof(TransactionId) * snapshot->subxcnt);
	snapshot->suboverflowed = cachedAnchor.snap.suboverflowed;
	snapshot->takenDuringRecovery = true;

	/* the snapshot remembers which registration it carries (dispatch) */
	snapshot->anchorOrdinal = ordinal;
}

/*
 * Is this backend an executor serving an anchored dispatch?  The
 * dispatcher announces the anchor in the transaction context it ships
 * (GP_OPT_ANCHORED_SNAPSHOT and the name), which setupQEDtxContext copied
 * into QEDtxContextInfo before the executor took any snapshot for it.
 */
bool
AnchorSnapshotDispatched(void)
{
	return Gp_role == GP_ROLE_EXECUTE &&
		isMppTxOptions_Anchored(QEDtxContextInfo.distributedTxnOptions);
}

/*
 * GetSnapshotData() publishes an executor writer's snapshot to its reader
 * gang before the snapshot funnel runs the installer; under an anchored
 * dispatch it hands that publication to the installer instead, so that
 * readers copy the anchored set.  The hand-off is per GetSnapshotData()
 * call: set when a publication was skipped, cleared at the top of the
 * next call and when the installer has published.
 */
static bool deferredPublication = false;

void
AnchorSnapshotSetDeferredPublication(bool deferred)
{
	deferredPublication = deferred;
}

/*
 * Executor side: every executor backend that serves an anchored dispatch
 * (writer, reader, cursor reader, the single segment of a direct dispatch,
 * the entry-db singleton) installs ITS OWN node's anchor of the dispatched
 * name, whatever DistributedTransactionContext it is in and whatever its
 * own mode GUC says: a SET does not reach a busy cursor gang, and the
 * dispatch is the only word on which cut the statement reads.  A reader
 * has just copied the writer's (already anchored) set from the shared
 * slot; installing again from the file gives the same set, and publishes
 * the reader's xmin, which readers never do otherwise, so that conflict
 * handling finds every process reading the anchor.
 */
static bool
installDispatchedAnchor(Snapshot snapshot)
{
	const char *name = QEDtxContextInfo.anchorName;
	TransactionId regXmin;
	uint64		ordinal;

	if (!isMppTxOptions_Anchored(QEDtxContextInfo.distributedTxnOptions))
		return false;

	if (!RecoveryInProgress() || !registryEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("anchored dispatch reached a segment that cannot install anchor snapshots"),
				 errdetail("The coordinator dispatched anchor snapshot \"%s\", but this segment is not in recovery or its anchor registry is disabled (whpg_max_anchor_snapshots = 0).",
						   printableName(name)),
				 errhint("Every node of an anchored standby cluster must be a hot standby with the anchor registry enabled.")));

	if (!AnchorSnapshotLookup(name, &regXmin, &ordinal))
		anchorNotRegisteredError(name);

	installRegisteredAnchor(snapshot, name, regXmin, ordinal);

	/*
	 * Single-layer visibility: under the anchor the executor never consults
	 * the distributed snapshot or the distributed log.  The distributed
	 * snapshot the dispatcher shipped has already done its two jobs, in
	 * GetSnapshotData(): selecting this executor's transaction context and
	 * advancing the distributed-log horizon.  Resetting the mapping also
	 * keeps CopySnapshot() and SerializeSnapshot() from carrying stale
	 * distributed arrays.
	 */
	SnapshotResetDslm(snapshot);

	/*
	 * A writer publishes the snapshot for its reader gang only now, with the
	 * anchor laid over it: GetSnapshotData() handed the publication over
	 * when it skipped it for this snapshot (a snapshot it would not have
	 * published, such as the latest or the catalog snapshot, is not
	 * published here either).
	 */
	if (deferredPublication)
	{
		deferredPublication = false;
		Assert(SharedLocalSnapshotSlot != NULL);
		updateSharedLocalSnapshot(&QEDtxContextInfo, DistributedTransactionContext,
								  snapshot, "AnchorSnapshotInstall");
	}
	return true;
}

bool
AnchorSnapshotInstall(Snapshot snapshot, bool pin)
{
	const char *name;
	TransactionId regXmin;
	uint64		ordinal;

	if (Gp_role == GP_ROLE_EXECUTE)
		return installDispatchedAnchor(snapshot);

	/*
	 * A transaction that pinned an anchor reads it until it ends, whatever
	 * the mode GUC says by now; every other snapshot follows the GUC.
	 */
	if (pinnedAnchor.pinned)
	{
		name = pinnedAnchor.name;
		if (!AnchorSnapshotLookup(name, &regXmin, &ordinal) ||
			ordinal != pinnedAnchor.ordinal)
			anchorPinnedGoneError();
	}
	else if (!anchoredReadApplies())
		return false;
	else
	{
		name = whpg_hot_standby_anchor_name;
		if (name == NULL || name[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("anchored read requires a published anchor snapshot"),
					 errdetail("whpg_hot_standby_anchor_name is not set on this hot standby."),
					 errhint("Wait for the next publication, or SET whpg_hot_standby_snapshot_mode = unanchored for this session.")));
		if (!AnchorSnapshotLookup(name, &regXmin, &ordinal))
			anchorNotRegisteredError(name);
	}

	installRegisteredAnchor(snapshot, name, regXmin, ordinal);

	if (pin && !pinnedAnchor.pinned)
	{
		strlcpy(pinnedAnchor.name, name, MAXFNAMELEN);
		pinnedAnchor.xmin = regXmin;
		pinnedAnchor.ordinal = ordinal;
		pinnedAnchor.pinned = true;
	}
	return true;
}

/*
 * Dispatch side: the name of the anchor a snapshot carries (its
 * anchorOrdinal), copied into name (MAXFNAMELEN bytes).  The registry is
 * consulted every time so that a snapshot whose anchor was retired,
 * evicted or invalidated after it was installed is refused here, before
 * the statement is dispatched: the executors could not install that
 * anchor anyway.
 */
void
AnchorSnapshotNameForDispatch(uint64 ordinal, char *name)
{
	bool		found = false;
	int			i;

	Assert(ordinal != 0);

	if (registryEnabled())
	{
		SpinLockAcquire(&anchorRegistry->lock);
		for (i = 0; i < anchorRegistry->capacity; i++)
		{
			AnchorRegistryEntry *e = &anchorRegistry->entries[i];

			if (e->valid && e->ordinal == ordinal)
			{
				strlcpy(name, e->rp_name, MAXFNAMELEN);
				found = true;
				break;
			}
		}
		SpinLockRelease(&anchorRegistry->lock);
	}

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("anchor snapshot of the snapshot being dispatched is no longer registered"),
				 errdetail("The anchor was retired, evicted or invalidated after this statement's snapshot installed it."),
				 errhint("Retry the statement; a REPEATABLE READ transaction must roll back first.")));
}

bool
AnchorSnapshotNameIsValid(const char *name)
{
	return anchorNameIsValid(name);
}

void
AnchorSnapshotValidatePinned(void)
{
	TransactionId xmin;
	uint64		ordinal;

	if (!pinnedAnchor.pinned)
		return;
	if (!AnchorSnapshotLookup(pinnedAnchor.name, &xmin, &ordinal) ||
		ordinal != pinnedAnchor.ordinal)
		anchorPinnedGoneError();
}

bool
AnchorSnapshotTransactionPinned(void)
{
	return pinnedAnchor.pinned;
}

void
AtEOXact_AnchorSnapshot(void)
{
	pinnedAnchor.pinned = false;
}


/* ------------------------------------------------------------------
 * Horizon
 * ------------------------------------------------------------------
 */

TransactionId
AnchorSnapshotOldestXmin(void)
{
	TransactionId oldest = InvalidTransactionId;
	int			i;

	if (!registryEnabled())
		return InvalidTransactionId;

	SpinLockAcquire(&anchorRegistry->lock);
	for (i = 0; i < anchorRegistry->capacity; i++)
	{
		AnchorRegistryEntry *e = &anchorRegistry->entries[i];

		if (e->valid &&
			(!TransactionIdIsValid(oldest) || TransactionIdPrecedes(e->xmin, oldest)))
			oldest = e->xmin;
	}
	SpinLockRelease(&anchorRegistry->lock);
	return oldest;
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
