#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "utils/memutils.h"

/*
 * The functions under test report every failure through ereport(WARNING)
 * and success through ereport(LOG); neither matters here and the logging
 * subsystem is not initialised in a unit test.
 */
#include "utils/elog.h"
#undef ereport
#define ereport(elevel, rest) ((void) 0)
#undef elog
#define elog(elevel, ...) ((void) 0)

/* Actual function body */
#include "../anchorsnapshot.c"

/*
 * A registry in ordinary memory: the module only ever dereferences the
 * pointer, so the unit tests never need real shared memory.
 */
static AnchorRegistryData *
makeRegistry(int capacity)
{
	Size		size = offsetof(AnchorRegistryData, entries) +
	sizeof(AnchorRegistryEntry) * capacity;
	AnchorRegistryData *reg = (AnchorRegistryData *) calloc(1, size);

	SpinLockInit(&reg->lock);
	reg->capacity = capacity;
	reg->next_ordinal = 1;
	reg->nvalid = 0;
	anchorRegistry = reg;
	/* the name buffer AnchorSnapshotStartup sizes to the registry */
	if (retireNames != NULL)
		free(retireNames);
	retireNames = (char *) calloc(capacity > 0 ? capacity : 1, MAXFNAMELEN);
	return reg;
}

/*
 * Every path that removes a registered entry runs the removal barrier
 * (ProcArrayLock taken exclusively once and released); the LWLock mock
 * fails the test on a barrier that is not expected here, so a removal that
 * finds nothing must not run one.
 */
static void
expectBarrier(int times)
{
	while (times-- > 0)
	{
		expect_value(LWLockAcquire, lock, ProcArrayLock);
		expect_value(LWLockAcquire, mode, LW_EXCLUSIVE);
		will_return(LWLockAcquire, true);
		expect_value(LWLockRelease, lock, ProcArrayLock);
		will_be_called(LWLockRelease);
	}
}

static void
test__anchorNameIsValid(void **state)
{
	char		longname[MAXFNAMELEN + 1];

	assert_true(anchorNameIsValid("rp_20260921T101500"));
	assert_true(anchorNameIsValid("a name with spaces"));
	assert_true(anchorNameIsValid("tmp"));
	assert_true(anchorNameIsValid("x.tmp.y"));

	assert_false(anchorNameIsValid(NULL));
	assert_false(anchorNameIsValid(""));
	assert_false(anchorNameIsValid("."));
	assert_false(anchorNameIsValid(".."));
	assert_false(anchorNameIsValid("../escape"));
	assert_false(anchorNameIsValid("a/b"));
	assert_false(anchorNameIsValid("a\\b"));
	assert_false(anchorNameIsValid("line\nbreak"));
	assert_false(anchorNameIsValid("tab\there"));
	assert_false(anchorNameIsValid("del\x7f"));
	assert_false(anchorNameIsValid("rp_1.tmp"));

	memset(longname, 'a', MAXFNAMELEN);
	longname[MAXFNAMELEN] = '\0';
	assert_false(anchorNameIsValid(longname));
	longname[MAXFNAMELEN - 1] = '\0';
	assert_true(anchorNameIsValid(longname));
}

static void
test__registry_register_lookup_full_duplicate(void **state)
{
	TransactionId xmin = InvalidTransactionId;

	makeRegistry(2);

	assert_false(AnchorSnapshotLookup("rp1", &xmin, NULL));
	assert_int_equal(registerAnchor("rp1", 100), REGISTER_OK);
	assert_true(AnchorSnapshotLookup("rp1", &xmin, NULL));
	assert_int_equal(xmin, 100);

	/* a duplicate is refused and the existing entry kept */
	assert_int_equal(registerAnchor("rp1", 200), REGISTER_DUPLICATE);
	assert_true(AnchorSnapshotLookup("rp1", &xmin, NULL));
	assert_int_equal(xmin, 100);

	assert_int_equal(registerAnchor("rp2", 110), REGISTER_OK);
	assert_int_equal(registerAnchor("rp3", 120), REGISTER_FULL);
	assert_false(AnchorSnapshotLookup("rp3", &xmin, NULL));

	/* invalidation frees the slot on the spot */
	expectBarrier(1);
	AnchorSnapshotInvalidate("rp1");
	assert_false(AnchorSnapshotLookup("rp1", &xmin, NULL));
	assert_int_equal(registerAnchor("rp3", 120), REGISTER_OK);
	assert_true(AnchorSnapshotLookup("rp3", &xmin, NULL));
	assert_int_equal(xmin, 120);

	free(anchorRegistry);
	anchorRegistry = NULL;
}

/*
 * Ordinals are never reused: the registration that would take the last
 * one is refused, and everything registered before it stays.
 */
static void
test__registry_ordinal_exhaustion(void **state)
{
	AnchorRegistryData *reg = makeRegistry(4);
	TransactionId xmin;
	uint32		ordinal;

	reg->next_ordinal = PG_UINT32_MAX - 1;
	assert_int_equal(registerAnchor("last", 100), REGISTER_OK);
	assert_true(AnchorSnapshotLookup("last", &xmin, &ordinal));
	assert_int_equal(ordinal, PG_UINT32_MAX - 1);

	assert_int_equal(registerAnchor("one_too_many", 110), REGISTER_EXHAUSTED);
	assert_int_equal(registerAnchorEvicting("one_too_many", 110), REGISTER_EXHAUSTED);
	assert_false(AnchorSnapshotLookup("one_too_many", &xmin, &ordinal));
	assert_true(AnchorSnapshotLookup("last", &xmin, &ordinal));
	assert_int_equal(reg->next_ordinal, PG_UINT32_MAX);

	free(reg);
	anchorRegistry = NULL;
}

static void
test__registry_disabled(void **state)
{
	TransactionId xmin;
	AnchorSnapshotInfo info[1];

	makeRegistry(0);
	assert_false(registryEnabled());
	assert_false(AnchorSnapshotLookup("rp1", &xmin, NULL));
	assert_int_equal(AnchorSnapshotList(info, 1), 0);
	free(anchorRegistry);
	anchorRegistry = NULL;
}

/*
 * The GUCs the sizing test changes, restored by its teardown: a failed
 * assertion longjmps out of the test, past any restore at its end.
 */
static bool savedHotStandby;
static int	savedMaxAnchors;

static void
saveSizingGUCs(void **state)
{
	savedHotStandby = EnableHotStandby;
	savedMaxAnchors = whpg_max_anchor_snapshots;
}

static void
restoreSizingGUCs(void **state)
{
	EnableHotStandby = savedHotStandby;
	whpg_max_anchor_snapshots = savedMaxAnchors;
}

/* A server started without hot standby sizes the registry to no entries. */
static void
test__registry_sized_only_for_hot_standby(void **state)
{
	Size		header = offsetof(AnchorRegistryData, entries);

	whpg_max_anchor_snapshots = 64;
	EnableHotStandby = false;
	assert_int_equal(registryCapacity(), 0);
	assert_int_equal(AnchorRegistryShmemSize(), header);

	EnableHotStandby = true;
	assert_int_equal(registryCapacity(), 64);
	assert_int_equal(AnchorRegistryShmemSize(),
					 header + 64 * sizeof(AnchorRegistryEntry));

	whpg_max_anchor_snapshots = 0;
	assert_int_equal(registryCapacity(), 0);
	assert_int_equal(AnchorRegistryShmemSize(), header);
}

static void
test__list_in_registration_order(void **state)
{
	AnchorSnapshotInfo info[4];
	int			n;

	makeRegistry(4);
	assert_int_equal(registerAnchor("c", 30), REGISTER_OK);
	assert_int_equal(registerAnchor("a", 10), REGISTER_OK);
	assert_int_equal(registerAnchor("b", 20), REGISTER_OK);
	/* free the first slot and reuse it: the ordinal, not the slot, orders */
	expectBarrier(1);
	AnchorSnapshotInvalidate("c");
	assert_int_equal(registerAnchor("d", 40), REGISTER_OK);

	n = AnchorSnapshotList(info, 4);
	assert_int_equal(n, 3);
	assert_string_equal(info[0].rp_name, "a");
	assert_string_equal(info[1].rp_name, "b");
	assert_string_equal(info[2].rp_name, "d");
	assert_int_equal(info[2].xmin, 40);

	/* a smaller buffer still reports the full count */
	n = AnchorSnapshotList(info, 1);
	assert_int_equal(n, 3);
	assert_string_equal(info[0].rp_name, "a");

	free(anchorRegistry);
	anchorRegistry = NULL;
}

/*
 * Publication retires every anchor registered before the newly named one;
 * an empty or unknown name retires nothing.
 */
static void
test__retire_on_publish(void **state)
{
	TransactionId xmin;

	makeRegistry(8);
	assert_int_equal(registerAnchor("rp1", 10), REGISTER_OK);
	assert_int_equal(registerAnchor("rp2", 20), REGISTER_OK);
	assert_int_equal(registerAnchor("rp3", 30), REGISTER_OK);
	assert_int_equal(registerAnchor("rp4", 40), REGISTER_OK);

	/* the boot value is recorded by AnchorSnapshotStartup; emulate it */
	strlcpy(lastSeenAnchorName, "", MAXFNAMELEN);
	lastSeenAnchorNameSet = true;

	/* unknown name: nothing retired */
	whpg_hot_standby_anchor_name = "nope";
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp1", &xmin, NULL));
	assert_true(AnchorSnapshotLookup("rp4", &xmin, NULL));

	/* publish rp3: rp1 and rp2 go, rp3 and the newer rp4 stay */
	whpg_hot_standby_anchor_name = "rp3";
	expectBarrier(1);
	AnchorSnapshotOnConfigReload();
	assert_false(AnchorSnapshotLookup("rp1", &xmin, NULL));
	assert_false(AnchorSnapshotLookup("rp2", &xmin, NULL));
	assert_true(AnchorSnapshotLookup("rp3", &xmin, NULL));
	assert_true(AnchorSnapshotLookup("rp4", &xmin, NULL));

	/* the same value again is not a publication */
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp3", &xmin, NULL));

	/* an empty name retires nothing either */
	whpg_hot_standby_anchor_name = "";
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp3", &xmin, NULL));
	assert_true(AnchorSnapshotLookup("rp4", &xmin, NULL));

	/* before Startup ran, a reload is ignored */
	lastSeenAnchorNameSet = false;
	whpg_hot_standby_anchor_name = "rp4";
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp3", &xmin, NULL));

	/*
	 * A reload naming an anchor not registered yet is completed by the
	 * export of that name: applyPublication is what the export calls.
	 */
	lastSeenAnchorNameSet = true;
	whpg_hot_standby_anchor_name = "rp5";
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp3", &xmin, NULL));
	assert_int_equal(registerAnchor("rp5", 50), REGISTER_OK);
	expectBarrier(1);
	assert_true(applyPublication("rp5"));
	assert_false(AnchorSnapshotLookup("rp3", &xmin, NULL));
	assert_false(AnchorSnapshotLookup("rp4", &xmin, NULL));
	assert_true(AnchorSnapshotLookup("rp5", &xmin, NULL));
	assert_false(applyPublication("unknown"));
	assert_false(applyPublication(""));

	free(anchorRegistry);
	anchorRegistry = NULL;
	whpg_hot_standby_anchor_name = NULL;
}

/*
 * A full registry gives up its oldest unpublished anchor for a new export;
 * the published anchor is never evicted.
 */
static void
test__evict_oldest_unpublished(void **state)
{
	TransactionId xmin;

	makeRegistry(3);
	assert_int_equal(registerAnchor("a", 10), REGISTER_OK);
	assert_int_equal(registerAnchor("b", 20), REGISTER_OK);
	assert_int_equal(registerAnchor("c", 30), REGISTER_OK);
	assert_int_equal(registerAnchor("full", 99), REGISTER_FULL);

	/* b is published: a (oldest) goes first, then c, never b */
	whpg_hot_standby_anchor_name = "b";
	expectBarrier(1);
	assert_true(evictOldestUnpublished("d"));
	assert_false(AnchorSnapshotLookup("a", &xmin, NULL));
	assert_true(AnchorSnapshotLookup("b", &xmin, NULL));
	assert_true(AnchorSnapshotLookup("c", &xmin, NULL));
	assert_int_equal(registerAnchor("d", 40), REGISTER_OK);
	assert_int_equal(registerAnchor("full", 99), REGISTER_FULL);

	expectBarrier(1);
	assert_true(evictOldestUnpublished("e"));
	assert_false(AnchorSnapshotLookup("c", &xmin, NULL));
	assert_true(AnchorSnapshotLookup("b", &xmin, NULL));
	assert_int_equal(registerAnchor("e", 50), REGISTER_OK);

	/* only the published anchor left: nothing to evict (and no barrier) */
	expectBarrier(2);
	assert_true(evictOldestUnpublished("f"));
	assert_false(AnchorSnapshotLookup("d", &xmin, NULL));
	assert_true(evictOldestUnpublished("f"));
	assert_false(AnchorSnapshotLookup("e", &xmin, NULL));
	assert_false(evictOldestUnpublished("f"));
	assert_true(AnchorSnapshotLookup("b", &xmin, NULL));

	/* no published anchor at all: plain oldest-first */
	whpg_hot_standby_anchor_name = "";
	expectBarrier(1);
	assert_true(evictOldestUnpublished("f"));
	assert_false(AnchorSnapshotLookup("b", &xmin, NULL));

	free(anchorRegistry);
	anchorRegistry = NULL;
	whpg_hot_standby_anchor_name = NULL;
}

/* Corrupt one file in place and expect the parser to refuse it. */
static void
expectRefused(const char *name, const char *content)
{
	char		path[MAXPGPATH];
	FILE	   *f;
	TimeLineID	tli;
	XLogRecPtr	lsn;
	TransactionId xmin;

	anchorFilePath(path, sizeof(path), name, false);
	f = fopen(path, "w");
	assert_true(f != NULL);
	assert_true(fputs(content, f) >= 0);
	fclose(f);
	assert_false(parseAnchorFile(name, &tli, &lsn, &xmin, NULL, NULL));
}

/*
 * File round trip in a scratch directory: the writer's output is what the
 * startup rebuild accepts, and every deviation from the grammar is refused.
 */
static void
test__file_roundtrip_and_validation(void **state)
{
	char		cwd[MAXPGPATH];
	char		tmpl[] = "/tmp/anchorsnapshot_test_XXXXXX";
	char	   *dir = mkdtemp(tmpl);
	TransactionId xids[3] = {101, 105, 106};
	TransactionId xmin = InvalidTransactionId;
	TimeLineID	tli = 0;
	XLogRecPtr	lsn = InvalidXLogRecPtr;
	char		path[MAXPGPATH];
	char		good[1024];
	char		bad[1024];
	FILE	   *f;
	char		line[128];
	long		size;
	char	   *crcline;

	assert_true(dir != NULL);
	assert_true(getcwd(cwd, sizeof(cwd)) != NULL);
	assert_int_equal(chdir(dir), 0);
	assert_int_equal(mkdir(ANCHOR_SNAPSHOT_DIR, 0700), 0);

	assert_true(writeAnchorFile("rp1", 2, (XLogRecPtr) 0x16B3D50, 101, 107,
								xids, 3, false));
	assert_true(parseAnchorFile("rp1", &tli, &lsn, &xmin, NULL, NULL));
	assert_int_equal(tli, 2);
	assert_true(lsn == (XLogRecPtr) 0x16B3D50);
	assert_int_equal(xmin, 101);

	/* the grammar, line by line */
	anchorFilePath(path, sizeof(path), "rp1", false);
	f = fopen(path, "r");
	assert_true(f != NULL);
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "fmt:2\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "rp_name:rp1\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "tli:2\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "lsn:0/16B3D50\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "xmin:101\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "xmax:107\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "xcnt:0\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "sof:0\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "sxcnt:3\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "sxp:101\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "sxp:105\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "sxp:106\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "rec:1\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_int_equal(strncmp(line, "crc:", 4), 0);
	assert_int_equal(strlen(line), 13);
	assert_true(fgets(line, sizeof(line), f) == NULL);
	fclose(f);

	/* keep the valid text for the corruption cases below */
	f = fopen(path, "r");
	assert_true(f != NULL);
	size = fread(good, 1, sizeof(good) - 1, f);
	assert_true(size > 13);
	good[size] = '\0';
	fclose(f);
	crcline = good + size - 13;

	/* an overflowed snapshot carries its xid list like any other */
	assert_true(writeAnchorFile("rp2", 1, (XLogRecPtr) 0x2000000, 200, 210,
								xids, 3, true));
	anchorFilePath(path, sizeof(path), "rp2", false);
	f = fopen(path, "r");
	assert_true(f != NULL);
	assert_true(fgets(line, sizeof(line), f) != NULL);	/* fmt */
	assert_true(fgets(line, sizeof(line), f) != NULL);	/* rp_name */
	assert_true(fgets(line, sizeof(line), f) != NULL);	/* tli */
	assert_true(fgets(line, sizeof(line), f) != NULL);	/* lsn */
	assert_true(fgets(line, sizeof(line), f) != NULL);	/* xmin */
	assert_true(fgets(line, sizeof(line), f) != NULL);	/* xmax */
	assert_true(fgets(line, sizeof(line), f) != NULL);	/* xcnt */
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "sof:1\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "sxcnt:3\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "sxp:101\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);	/* sxp:105 */
	assert_true(fgets(line, sizeof(line), f) != NULL);	/* sxp:106 */
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "rec:1\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_int_equal(strncmp(line, "crc:", 4), 0);
	fclose(f);
	assert_true(parseAnchorFile("rp2", &tli, &lsn, &xmin, NULL, NULL));
	assert_int_equal(xmin, 200);

	/* a file whose name line names another anchor is refused */
	anchorFilePath(path, sizeof(path), "rp3", false);
	assert_int_equal(rename("pg_anchor_snapshots/rp2", path), 0);
	assert_false(parseAnchorFile("rp3", &tli, &lsn, &xmin, NULL, NULL));

	/* the exact text again is accepted (crc intact) */
	expectRefused("rp1", "");
	{
		char	   *p;

		anchorFilePath(path, sizeof(path), "rp1", false);
		f = fopen(path, "w");
		assert_true(fputs(good, f) >= 0);
		fclose(f);
		assert_true(parseAnchorFile("rp1", &tli, &lsn, &xmin, NULL, NULL));

		/* a flipped digit inside an sxp line: crc catches it */
		strcpy(bad, good);
		p = strstr(bad, "sxp:105");
		assert_true(p != NULL);
		p[4] = '2';
		expectRefused("rp1", bad);

		/* a stray byte after the crc line */
		strcpy(bad, good);
		strcat(bad, "x");
		expectRefused("rp1", bad);

		/* truncated */
		strcpy(bad, good);
		bad[20] = '\0';
		expectRefused("rp1", bad);

		/* sxcnt says 3, only two sxp lines follow (crc recomputed to isolate the count check) */
		{
			pg_crc32c	crc;
			char	   *q;

			strcpy(bad, good);
			q = strstr(bad, "sxp:106\n");
			assert_true(q != NULL);
			memmove(q, q + 8, strlen(q + 8) + 1);
			INIT_CRC32C(crc);
			COMP_CRC32C(crc, bad, strlen(bad) - 13);
			FIN_CRC32C(crc);
			sprintf(bad + strlen(bad) - 13, "crc:%08X\n", crc);
			expectRefused("rp1", bad);

			/* a missing field with a valid crc */
			strcpy(bad, good);
			q = strstr(bad, "xcnt:0\n");
			assert_true(q != NULL);
			memmove(q, q + 7, strlen(q + 7) + 1);
			INIT_CRC32C(crc);
			COMP_CRC32C(crc, bad, strlen(bad) - 13);
			FIN_CRC32C(crc);
			sprintf(bad + strlen(bad) - 13, "crc:%08X\n", crc);
			expectRefused("rp1", bad);

			/* an unknown format version with a valid crc */
			strcpy(bad, good);
			bad[4] = '7';
			INIT_CRC32C(crc);
			COMP_CRC32C(crc, bad, strlen(bad) - 13);
			FIN_CRC32C(crc);
			sprintf(bad + strlen(bad) - 13, "crc:%08X\n", crc);
			expectRefused("rp1", bad);
		}
		(void) crcline;
	}

	/* a missing file is refused */
	assert_false(parseAnchorFile("rp9", &tli, &lsn, &xmin, NULL, NULL));

	/* the sweep keeps only the named file */
	assert_true(writeAnchorFile("keep", 1, (XLogRecPtr) 0x3000000, 300, 301,
								xids, 0, false));
	sweepAnchorFiles("keep");
	assert_int_equal(access("pg_anchor_snapshots/keep", F_OK), 0);
	assert_int_equal(access("pg_anchor_snapshots/rp1", F_OK), -1);
	assert_int_equal(access("pg_anchor_snapshots/rp3", F_OK), -1);
	sweepAnchorFiles(NULL);
	assert_int_equal(access("pg_anchor_snapshots/keep", F_OK), -1);

	assert_int_equal(rmdir(ANCHOR_SNAPSHOT_DIR), 0);
	assert_int_equal(chdir(cwd), 0);
	assert_int_equal(rmdir(dir), 0);
}

/*
 * The installer's reader: the xid set comes back through the out struct,
 * an overflowed file keeps its xid list (the flag only changes how the
 * reader maps subtransactions), and with a StringInfo the reason for a
 * refusal is handed back instead of being logged.
 */
static void
test__parse_returns_xid_set(void **state)
{
	char		cwd[MAXPGPATH];
	char		tmpl[] = "/tmp/anchorsnapshot_test_XXXXXX";
	char	   *dir = mkdtemp(tmpl);
	TransactionId xids[3] = {101, 105, 106};
	TransactionId xmin = InvalidTransactionId;
	TimeLineID	tli = 0;
	XLogRecPtr	lsn = InvalidXLogRecPtr;
	AnchorFileSnapshot out;
	StringInfoData why;

	assert_true(dir != NULL);
	assert_true(getcwd(cwd, sizeof(cwd)) != NULL);
	assert_int_equal(chdir(dir), 0);
	assert_int_equal(mkdir(ANCHOR_SNAPSHOT_DIR, 0700), 0);

	assert_true(writeAnchorFile("rp1", 2, (XLogRecPtr) 0x16B3D50, 101, 107,
								xids, 3, false));
	memset(&out, 0x7f, sizeof(out));
	assert_true(parseAnchorFile("rp1", &tli, &lsn, &xmin, &out, NULL));
	assert_int_equal(out.xmin, 101);
	assert_int_equal(out.xmax, 107);
	assert_false(out.suboverflowed);
	assert_int_equal(out.subxcnt, 3);
	assert_true(out.subxip != NULL);
	assert_int_equal(out.subxip[0], 101);
	assert_int_equal(out.subxip[1], 105);
	assert_int_equal(out.subxip[2], 106);
	pfree(out.subxip);

	/* an overflowed snapshot carries its xid list too */
	assert_true(writeAnchorFile("rp2", 2, (XLogRecPtr) 0x16B3E00, 200, 210,
								xids, 3, true));
	memset(&out, 0x7f, sizeof(out));
	assert_true(parseAnchorFile("rp2", &tli, &lsn, &xmin, &out, NULL));
	assert_int_equal(out.xmin, 200);
	assert_int_equal(out.xmax, 210);
	assert_true(out.suboverflowed);
	assert_int_equal(out.subxcnt, 3);
	assert_true(out.subxip != NULL);
	assert_int_equal(out.subxip[2], 106);
	pfree(out.subxip);

	/* an empty xid list is fine too */
	assert_true(writeAnchorFile("rp3", 2, (XLogRecPtr) 0x16B3F00, 300, 300,
								xids, 0, false));
	memset(&out, 0x7f, sizeof(out));
	assert_true(parseAnchorFile("rp3", &tli, &lsn, &xmin, &out, NULL));
	assert_int_equal(out.subxcnt, 0);
	assert_true(out.subxip == NULL);

	/* the reason goes to the caller when asked for */
	initStringInfo(&why);
	assert_false(parseAnchorFile("rp9", &tli, &lsn, &xmin, &out, &why));
	assert_true(strstr(why.data, "could not open anchor snapshot file") != NULL);
	resetStringInfo(&why);
	assert_false(parseAnchorFile("rp1x", &tli, &lsn, &xmin, NULL, &why));
	assert_true(strstr(why.data, "could not open") != NULL);
	resetStringInfo(&why);
	/* the wrong name for an existing file: a grammar refusal with its reason */
	assert_int_equal(rename("pg_anchor_snapshots/rp1", "pg_anchor_snapshots/other"), 0);
	assert_false(parseAnchorFile("other", &tli, &lsn, &xmin, &out, &why));
	assert_true(strstr(why.data, "refused: rp_name line does not name this restore point") != NULL);
	pfree(why.data);

	sweepAnchorFiles(NULL);
	assert_int_equal(rmdir(ANCHOR_SNAPSHOT_DIR), 0);
	assert_int_equal(chdir(cwd), 0);
	assert_int_equal(rmdir(dir), 0);
}

/* The pg_subtrans horizon follows the oldest registered anchor. */
static void
test__oldest_xmin_and_restart_rule(void **state)
{
	AnchorRegistryData *reg = makeRegistry(4);
	TransactionId xmin;
	uint32		ord_a;
	uint32		ord_a2;

	assert_int_equal(AnchorSnapshotOldestXmin(), InvalidTransactionId);

	assert_int_equal(registerAnchor("a", 400), REGISTER_OK);
	assert_int_equal(registerAnchor("b", 300), REGISTER_OK);
	assert_int_equal(registerAnchor("c", 450), REGISTER_OK);
	assert_int_equal(AnchorSnapshotOldestXmin(), 300);

	/* an invalidated entry no longer holds the horizon */
	expectBarrier(1);
	AnchorSnapshotInvalidate("b");
	assert_int_equal(AnchorSnapshotOldestXmin(), 400);

	/* a name registered again is a new registration: the ordinal moves */
	assert_true(AnchorSnapshotLookup("a", &xmin, &ord_a));
	expectBarrier(1);
	AnchorSnapshotInvalidate("a");
	assert_int_equal(registerAnchor("a", 400), REGISTER_OK);
	assert_true(AnchorSnapshotLookup("a", &xmin, &ord_a2));
	assert_int_equal(xmin, 400);
	assert_true(ord_a2 > ord_a);

	/* a disabled registry holds nothing */
	reg->capacity = 0;
	assert_int_equal(AnchorSnapshotOldestXmin(), InvalidTransactionId);

	anchorRegistry = NULL;
	free(reg);

	/*
	 * Restart rule: an overflowed anchor survives only when its xid range
	 * ends at or before the start checkpoint's oldest active xid; a
	 * non-overflowed anchor and an unknown oldest active xid always do.
	 */
	{
		/* StartupSUBTRANS zeroes whole pages from oldestActiveXid's page on */
		TransactionId perPage = BLCKSZ / sizeof(SubTransData);
		TransactionId oldest = 10 * perPage + 100;	/* on page 10 */

		assert_true(anchorSurvivesStart(false, oldest + 400, oldest));
		assert_true(anchorSurvivesStart(true, oldest + 400, InvalidTransactionId));
		/* past the oldest active xid: refused */
		assert_false(anchorSurvivesStart(true, oldest + 1, oldest));
		/* at or below it but on the same page: refused all the same */
		assert_false(anchorSurvivesStart(true, oldest, oldest));
		assert_false(anchorSurvivesStart(true, oldest - 1, oldest));
		assert_false(anchorSurvivesStart(true, 10 * perPage + 1, oldest));
		/* the range ends on the page before: its pages were spared */
		assert_true(anchorSurvivesStart(true, 10 * perPage, oldest));
		assert_true(anchorSurvivesStart(true, 9 * perPage + 5, oldest));
		assert_true(anchorSurvivesStart(true, perPage, oldest));
	}
}

/*
 * Conflict linkage: a replayed cleanup record invalidates exactly the
 * anchors whose xmin is at or below its horizon, the published one
 * included, runs the barrier once for the lot, and runs nothing when no
 * anchor qualifies, when the horizon is invalid, or when the registry is
 * off and no kept anchor exists.
 */
static void
test__cleanup_record_invalidates_at_or_below(void **state)
{
	AnchorRegistryData *reg = makeRegistry(4);
	TransactionId xmin;
	RelFileNode node = {1663, 12345, 16384};

	assert_int_equal(registerAnchor("a", 400), REGISTER_OK);
	assert_int_equal(registerAnchor("b", 300), REGISTER_OK);
	assert_int_equal(registerAnchor("c", 450), REGISTER_OK);
	whpg_hot_standby_anchor_name = "a";

	/* below every xmin: nothing happens, no barrier */
	AnchorSnapshotOnCleanupRecord(299, node, (XLogRecPtr) 5000);
	assert_true(AnchorSnapshotLookup("b", &xmin, NULL));

	/* an invalid horizon is not a conflict */
	AnchorSnapshotOnCleanupRecord(InvalidTransactionId, node, (XLogRecPtr) 5000);
	assert_true(AnchorSnapshotLookup("b", &xmin, NULL));

	/* at the published anchor's xmin: a (equal) and b (below) go, c stays */
	expectBarrier(1);
	AnchorSnapshotOnCleanupRecord(400, node, (XLogRecPtr) 5000);
	assert_false(AnchorSnapshotLookup("a", &xmin, NULL));
	assert_false(AnchorSnapshotLookup("b", &xmin, NULL));
	assert_true(AnchorSnapshotLookup("c", &xmin, NULL));
	assert_int_equal(AnchorSnapshotOldestXmin(), 450);

	/* the freed slots are reusable */
	assert_int_equal(registerAnchor("d", 500), REGISTER_OK);

	anchorRegistry = NULL;
	free(reg);
	whpg_hot_standby_anchor_name = NULL;
}

/*
 * The kept anchor (a file a start could not register) is removed by the
 * cleanup hook once the record lies past its restore point and the
 * horizon reaches its xmin; it has no entry, so no barrier runs for it.
 * The clear at the end of recovery forgets it too.
 */
static void
test__kept_anchor_gate(void **state)
{
	AnchorRegistryData *reg = makeRegistry(0);
	RelFileNode node = {1663, 12345, 16384};

	keptSet = true;
	strlcpy(keptName, "k", MAXFNAMELEN);
	keptXmin = 400;
	keptLSN = (XLogRecPtr) 1000;

	/* the record precedes the restore point: never a conflict */
	AnchorSnapshotOnCleanupRecord(500, node, (XLogRecPtr) 900);
	assert_true(keptSet);
	AnchorSnapshotOnCleanupRecord(500, node, (XLogRecPtr) 1000);
	assert_true(keptSet);
	/* past it, below the xmin: kept */
	AnchorSnapshotOnCleanupRecord(399, node, (XLogRecPtr) 2000);
	assert_true(keptSet);
	/* past it, at the xmin: removed */
	AnchorSnapshotOnCleanupRecord(400, node, (XLogRecPtr) 2000);
	assert_false(keptSet);

	/* the clear at the end of recovery forgets a kept anchor */
	keptSet = true;
	AnchorSnapshotClearAll();
	assert_false(keptSet);

	anchorRegistry = NULL;
	free(reg);

	/* the clear runs the barrier only when it invalidates an entry */
	reg = makeRegistry(2);
	AnchorSnapshotClearAll();
	assert_int_equal(registerAnchor("a", 400), REGISTER_OK);
	expectBarrier(1);
	AnchorSnapshotClearAll();
	assert_int_equal(AnchorSnapshotOldestXmin(), InvalidTransactionId);
	anchorRegistry = NULL;
	free(reg);
}

/*
 * Every path that removes an entry lowers the count, registration raises
 * it.
 */
static void
test__removal_count(void **state)
{
	AnchorRegistryData *reg = makeRegistry(4);
	RelFileNode node = {1663, 12345, 16384};
	TransactionId xmin;

	assert_int_equal(registerAnchor("a", 400), REGISTER_OK);
	assert_int_equal(registerAnchor("b", 300), REGISTER_OK);
	assert_int_equal(registerAnchor("c", 450), REGISTER_OK);
	assert_int_equal(reg->nvalid, 3);

	/* a cleanup record: b goes */
	whpg_hot_standby_anchor_name = "a";
	expectBarrier(1);
	AnchorSnapshotOnCleanupRecord(300, node, (XLogRecPtr) 5000);
	assert_int_equal(reg->nvalid, 2);

	/* eviction of the oldest unpublished: c goes (a is published) */
	expectBarrier(1);
	assert_true(evictOldestUnpublished("d"));
	assert_int_equal(reg->nvalid, 1);

	/* registration again */
	assert_int_equal(registerAnchor("d", 500), REGISTER_OK);
	assert_int_equal(reg->nvalid, 2);

	/* publication of d retires a */
	whpg_hot_standby_anchor_name = "d";
	expectBarrier(1);
	assert_true(applyPublication("d"));
	assert_false(AnchorSnapshotLookup("a", &xmin, NULL));
	assert_int_equal(reg->nvalid, 1);

	/* removal by name */
	expectBarrier(1);
	AnchorSnapshotInvalidate("d");
	assert_int_equal(reg->nvalid, 0);

	/* the clear at the end of recovery */
	assert_int_equal(registerAnchor("e", 600), REGISTER_OK);
	expectBarrier(1);
	AnchorSnapshotClearAll();
	assert_int_equal(reg->nvalid, 0);

	anchorRegistry = NULL;
	free(reg);
	whpg_hot_standby_anchor_name = NULL;
}

/*
 * With nothing registered and no kept file the hook returns before the
 * scan.  An entry planted as valid while the count says zero survives a
 * horizon that would invalidate it, which only the early return explains.
 */
static void
test__hook_skips_an_empty_registry(void **state)
{
	AnchorRegistryData *reg = makeRegistry(4);
	RelFileNode node = {1663, 12345, 16384};
	TransactionId xmin;

	keptSet = false;
	assert_int_equal(registerAnchor("a", 400), REGISTER_OK);
	reg->nvalid = 0;			/* the plant: the scan would find "a" */

	AnchorSnapshotOnCleanupRecord(1000, node, (XLogRecPtr) 5000);
	assert_true(AnchorSnapshotLookup("a", &xmin, NULL));

	anchorRegistry = NULL;
	free(reg);
}

/*
 * The relation check, with the catalog behind the seams.  The fakes model
 * one relation whose pg_class row the anchor sees at tid (1,1) with file
 * 5000; the "current" row and the chain walk are scripted per step.
 */
static AnchorRelVersion fakeAnchored;
static int	fakeAnchoredCalls;
static Oid	fakeCurNode;
static ItemPointerData fakeCurTid;
static bool fakeWalkResult;
static int	fakeWalkCalls;

static bool
fakeAnchoredRelationVersion(Oid relid, Snapshot snap, AnchorRelVersion *v)
{
	fakeAnchoredCalls++;
	*v = fakeAnchored;
	return v->exists;
}

static void
fakeCurrentRelationVersion(Oid relid, Oid *relfilenode, ItemPointer tid)
{
	*relfilenode = fakeCurNode;
	*tid = fakeCurTid;
}

static bool
fakeRelationVersionsKeepFile(Oid relid, Oid relfilenode, ItemPointerData from, ItemPointerData to)
{
	fakeWalkCalls++;
	assert_int_equal(relfilenode, fakeAnchored.relfilenode);
	assert_true(ItemPointerEquals(&from, &fakeAnchored.tid));
	assert_true(ItemPointerEquals(&to, &fakeCurTid));
	return fakeWalkResult;
}

static void
test__relation_verdicts(void **state)
{
	SnapshotData snapA;
	SnapshotData snapB;

	anchoredRelationVersionFn = fakeAnchoredRelationVersion;
	currentRelationVersionFn = fakeCurrentRelationVersion;
	relationVersionsKeepFileFn = fakeRelationVersionsKeepFile;
	if (anchorRelCache != NULL)
	{
		hash_destroy(anchorRelCache);
		anchorRelCache = NULL;
	}

	MemSet(&snapA, 0, sizeof(snapA));
	snapA.anchorOrdinal = 7;
	MemSet(&snapB, 0, sizeof(snapB));
	snapB.anchorOrdinal = 8;

	fakeAnchored.exists = true;
	fakeAnchored.relfilenode = 5000;
	ItemPointerSet(&fakeAnchored.tid, 1, 1);
	fakeAnchoredCalls = fakeWalkCalls = 0;

	/* the anchor's row version is the current one: no walk */
	fakeCurNode = 5000;
	ItemPointerSet(&fakeCurTid, 1, 1);
	assert_int_equal(anchorRelationVerdict(16384, &snapA, snapA.anchorOrdinal), ANCHOR_REL_OK);
	assert_int_equal(anchorRelationVerdict(16384, &snapA, snapA.anchorOrdinal), ANCHOR_REL_OK);
	assert_int_equal(fakeAnchoredCalls, 1);	/* cached per (relation, anchor) */
	assert_int_equal(fakeWalkCalls, 0);

	/* a rewrite: another file */
	fakeCurNode = 5001;
	ItemPointerSet(&fakeCurTid, 1, 2);
	assert_int_equal(anchorRelationVerdict(16384, &snapA, snapA.anchorOrdinal), ANCHOR_REL_REWRITTEN);
	assert_int_equal(fakeWalkCalls, 0);

	/* the row updated, same file (RENAME): the chain is walked once */
	fakeCurNode = 5000;
	ItemPointerSet(&fakeCurTid, 1, 3);
	fakeWalkResult = true;
	assert_int_equal(anchorRelationVerdict(16384, &snapA, snapA.anchorOrdinal), ANCHOR_REL_OK);
	assert_int_equal(anchorRelationVerdict(16384, &snapA, snapA.anchorOrdinal), ANCHOR_REL_OK);
	assert_int_equal(fakeWalkCalls, 1);

	/* updated again: walked again, to the new current version */
	ItemPointerSet(&fakeCurTid, 2, 1);
	assert_int_equal(anchorRelationVerdict(16384, &snapA, snapA.anchorOrdinal), ANCHOR_REL_OK);
	assert_int_equal(fakeWalkCalls, 2);

	/* same number after a rewrite and back (or the oid reused): the walk says no */
	ItemPointerSet(&fakeCurTid, 2, 2);
	fakeWalkResult = false;
	assert_int_equal(anchorRelationVerdict(16384, &snapA, snapA.anchorOrdinal), ANCHOR_REL_REWRITTEN);
	assert_int_equal(fakeWalkCalls, 3);
	/* the failed walk is not remembered as verified */
	assert_int_equal(anchorRelationVerdict(16384, &snapA, snapA.anchorOrdinal), ANCHOR_REL_REWRITTEN);
	assert_int_equal(fakeWalkCalls, 4);

	/* another anchor is another cache entry */
	assert_int_equal(fakeAnchoredCalls, 1);
	ItemPointerSet(&fakeAnchored.tid, 2, 2);
	assert_int_equal(anchorRelationVerdict(16384, &snapB, snapB.anchorOrdinal), ANCHOR_REL_OK);
	assert_int_equal(fakeAnchoredCalls, 2);

	/* a relation the anchor does not see */
	fakeAnchored.exists = false;
	assert_int_equal(anchorRelationVerdict(16385, &snapA, snapA.anchorOrdinal), ANCHOR_REL_ABSENT);
	assert_int_equal(anchorRelationVerdict(16385, &snapA, snapA.anchorOrdinal), ANCHOR_REL_ABSENT);
	assert_int_equal(fakeAnchoredCalls, 3);

	hash_destroy(anchorRelCache);
	anchorRelCache = NULL;
	anchoredRelationVersionFn = anchoredRelationVersion;
	currentRelationVersionFn = currentRelationVersion;
	relationVersionsKeepFileFn = relationVersionsKeepFile;
}

/*
 * The membership check of a parent, with the catalog behind the seams: the
 * anchor's children are fixed per test step, the current ones scripted.
 */
static AnchorChild fakeAnchoredKids[4];
static int	fakeAnchoredKidCount;
static int	fakeAnchoredKidCalls;
static AnchorChild fakeCurrentKids[4];
static int	fakeCurrentKidCount;

static AnchorChild *
fakeAnchoredChildren(Oid parent, Snapshot snap, MemoryContext cxt, int *n)
{
	AnchorChild *kids = MemoryContextAlloc(cxt, sizeof(fakeAnchoredKids));

	fakeAnchoredKidCalls++;
	memcpy(kids, fakeAnchoredKids, sizeof(fakeAnchoredKids));
	*n = fakeAnchoredKidCount;
	return kids;
}

static AnchorChild *
fakeCurrentChildren(Oid parent, int *n)
{
	AnchorChild *kids = palloc(sizeof(fakeCurrentKids));

	memcpy(kids, fakeCurrentKids, sizeof(fakeCurrentKids));
	*n = fakeCurrentKidCount;
	return kids;
}

/* the fakes' bounds are literals: free only the array */
static void
fakeFreeChildren(AnchorChild *children, int n)
{
	pfree(children);
}

static void
setKids(AnchorChild *kids, int *count, int n, Oid first, const char *bound)
{
	int			i;

	*count = n;
	for (i = 0; i < n; i++)
	{
		kids[i].relid = first + i;
		kids[i].bound = (char *) bound;
	}
}

static void
test__inheritance_verdicts(void **state)
{
	SnapshotData snapA;
	SnapshotData snapB;

	anchoredChildrenFn = fakeAnchoredChildren;
	currentChildrenFn = fakeCurrentChildren;
	freeChildrenFn = fakeFreeChildren;
	if (anchorInhCache != NULL)
	{
		hash_destroy(anchorInhCache);
		anchorInhCache = NULL;
	}
	MemSet(&snapA, 0, sizeof(snapA));
	snapA.anchorOrdinal = 7;
	MemSet(&snapB, 0, sizeof(snapB));
	snapB.anchorOrdinal = 8;
	fakeAnchoredKidCalls = 0;

	/* two partitions at the anchor, the same two now */
	setKids(fakeAnchoredKids, &fakeAnchoredKidCount, 2, 20000, "FOR VALUES FROM (1) TO (11)");
	setKids(fakeCurrentKids, &fakeCurrentKidCount, 2, 20000, "FOR VALUES FROM (1) TO (11)");
	assert_true(anchorInheritanceUnchanged(16384, &snapA, snapA.anchorOrdinal));
	assert_true(anchorInheritanceUnchanged(16384, &snapA, snapA.anchorOrdinal));
	assert_int_equal(fakeAnchoredKidCalls, 1);	/* cached per (parent, anchor) */

	/*
	 * A match holds until another invalidation is accepted: replay sends
	 * one for every catalog change.  Every change below comes with one.
	 */
	fakeCurrentKidCount = 1;
	assert_true(anchorInheritanceUnchanged(16384, &snapA, snapA.anchorOrdinal));

	/* a partition detached or dropped */
	SharedInvalidMessageCounter++;
	assert_false(anchorInheritanceUnchanged(16384, &snapA, snapA.anchorOrdinal));

	/* one attached */
	SharedInvalidMessageCounter++;
	setKids(fakeCurrentKids, &fakeCurrentKidCount, 3, 20000, "FOR VALUES FROM (1) TO (11)");
	assert_false(anchorInheritanceUnchanged(16384, &snapA, snapA.anchorOrdinal));

	/* one exchanged: as many children, another oid */
	SharedInvalidMessageCounter++;
	setKids(fakeCurrentKids, &fakeCurrentKidCount, 2, 20000, "FOR VALUES FROM (1) TO (11)");
	fakeCurrentKids[1].relid = 30000;
	assert_false(anchorInheritanceUnchanged(16384, &snapA, snapA.anchorOrdinal));

	/* the same children, a bound changed (detached and attached again) */
	SharedInvalidMessageCounter++;
	setKids(fakeCurrentKids, &fakeCurrentKidCount, 2, 20000, "FOR VALUES FROM (1) TO (11)");
	fakeCurrentKids[1].bound = "FOR VALUES FROM (1) TO (21)";
	assert_false(anchorInheritanceUnchanged(16384, &snapA, snapA.anchorOrdinal));

	/* a partition that became a traditional inheritance child, or back */
	SharedInvalidMessageCounter++;
	fakeCurrentKids[1].bound = NULL;
	assert_false(anchorInheritanceUnchanged(16384, &snapA, snapA.anchorOrdinal));

	/* back to the anchor's shape: unchanged again */
	SharedInvalidMessageCounter++;
	fakeCurrentKids[1].bound = "FOR VALUES FROM (1) TO (11)";
	assert_true(anchorInheritanceUnchanged(16384, &snapA, snapA.anchorOrdinal));
	assert_int_equal(fakeAnchoredKidCalls, 1);

	/* traditional inheritance: no bounds on either side */
	setKids(fakeAnchoredKids, &fakeAnchoredKidCount, 1, 21000, NULL);
	setKids(fakeCurrentKids, &fakeCurrentKidCount, 1, 21000, NULL);
	assert_true(anchorInheritanceUnchanged(16385, &snapA, snapA.anchorOrdinal));
	/* the anchor's children, for the check to descend into */
	{
		AnchorChild *kids;
		int			n;

		kids = anchorInheritanceChildren(16385, snapA.anchorOrdinal, &n);
		assert_int_equal(n, 1);
		assert_int_equal(kids[0].relid, 21000);
		kids = anchorInheritanceChildren(16386, snapA.anchorOrdinal, &n);
		assert_int_equal(n, 0);
		assert_true(kids == NULL);
	}
	/* every child gone (NO INHERIT) */
	SharedInvalidMessageCounter++;
	fakeCurrentKidCount = 0;
	assert_false(anchorInheritanceUnchanged(16385, &snapA, snapA.anchorOrdinal));

	/* another anchor is another cache entry, read afresh */
	setKids(fakeAnchoredKids, &fakeAnchoredKidCount, 0, 0, NULL);
	assert_true(anchorInheritanceUnchanged(16385, &snapB, snapB.anchorOrdinal));
	assert_int_equal(fakeAnchoredKidCalls, 3);

	hash_destroy(anchorInhCache);
	anchorInhCache = NULL;
	anchoredChildrenFn = anchoredChildren;
	currentChildrenFn = currentChildren;
	freeChildrenFn = freeChildren;
}

/*
 * A snapshot's anchorOrdinal counts only up to the highest ordinal this
 * backend has installed: a SnapshotData built by hand, its field never
 * zeroed, must not look anchored to the dispatch, export and relation-open
 * checks.
 */
static void
test__ordinal_needs_an_install(void **state)
{
	GpRoleValue savedRole = Gp_role;
	TransactionId savedXmins[4] = {TransactionXmin, RecentXmin,
	RecentGlobalXmin, RecentGlobalDataXmin};
	SnapshotData garbage;
	SnapshotData snap;

	anchorHighestInstalled = 0;
	MemSet(&garbage, 0x7f, sizeof(garbage));
	assert_int_equal(AnchorSnapshotOrdinal(&garbage), 0);

	/* nothing prepared: no install, the field still does not count */
	MemSet(&snap, 0, sizeof(snap));
	preparedAnchor.pending = false;
	assert_false(AnchorSnapshotInstall(&snap));
	assert_int_equal(AnchorSnapshotOrdinal(&garbage), 0);

	/* an install makes ordinals up to its own count */
	Gp_role = GP_ROLE_DISPATCH;
	MemSet(&cachedAnchor, 0, sizeof(cachedAnchor));
	cachedAnchor.valid = true;
	cachedAnchor.ordinal = 7;
	cachedAnchor.snap.xmin = 100;
	cachedAnchor.snap.xmax = 110;
	MemSet(&preparedAnchor, 0, sizeof(preparedAnchor));
	preparedAnchor.pending = true;
	preparedAnchor.ordinal = 7;
	preparedAnchor.xmin = 100;
	assert_true(AnchorSnapshotInstall(&snap));
	assert_int_equal(snap.anchorOrdinal, 7);
	assert_int_equal(AnchorSnapshotOrdinal(&snap), 7);

	/* above the highest install: ignored */
	snap.anchorOrdinal = 8;
	assert_int_equal(AnchorSnapshotOrdinal(&snap), 0);
	assert_int_equal(AnchorSnapshotOrdinal(&garbage), 0);

	/* an older anchor of this backend still counts, and so would garbage in range */
	snap.anchorOrdinal = 5;
	assert_int_equal(AnchorSnapshotOrdinal(&snap), 5);
	snap.anchorOrdinal = 0;
	assert_int_equal(AnchorSnapshotOrdinal(&snap), 0);

	MemSet(&cachedAnchor, 0, sizeof(cachedAnchor));
	MemSet(&preparedAnchor, 0, sizeof(preparedAnchor));
	anchorHighestInstalled = 0;
	Gp_role = savedRole;
	TransactionXmin = savedXmins[0];
	RecentXmin = savedXmins[1];
	RecentGlobalXmin = savedXmins[2];
	RecentGlobalDataXmin = savedXmins[3];
}

int
main(int argc, char *argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
		unit_test(test__anchorNameIsValid),
		unit_test(test__registry_register_lookup_full_duplicate),
		unit_test(test__registry_ordinal_exhaustion),
		unit_test(test__registry_disabled),
		unit_test_setup_teardown(test__registry_sized_only_for_hot_standby,
								 saveSizingGUCs, restoreSizingGUCs),
		unit_test(test__list_in_registration_order),
		unit_test(test__retire_on_publish),
		unit_test(test__evict_oldest_unpublished),
		unit_test(test__file_roundtrip_and_validation),
		unit_test(test__parse_returns_xid_set),
		unit_test(test__oldest_xmin_and_restart_rule),
		unit_test(test__cleanup_record_invalidates_at_or_below),
		unit_test(test__kept_anchor_gate),
		unit_test(test__removal_count),
		unit_test(test__hook_skips_an_empty_registry),
		unit_test(test__relation_verdicts),
		unit_test(test__inheritance_verdicts),
		unit_test(test__ordinal_needs_an_install),
	};

	MemoryContextInit();

	return run_tests(tests);
}
