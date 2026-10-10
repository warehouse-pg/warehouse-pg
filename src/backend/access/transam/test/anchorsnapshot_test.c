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
	pg_atomic_init_u32(&reg->dropOrdinalHorizon, 0);
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
 * A commit that drops relation files invalidates every registered anchor:
 * every commit replayed after a registration belongs to a transaction the
 * anchor holds as in progress or not yet started, so its xid is at or
 * above every registered xmin.
 */
static void
test__relfilenode_drop_invalidates_all(void **state)
{
	AnchorRegistryData *reg = makeRegistry(4);
	TransactionId xmin;

	assert_int_equal(registerAnchor("a", 400), REGISTER_OK);
	assert_int_equal(registerAnchor("c", 450), REGISTER_OK);

	expectBarrier(1);
	AnchorSnapshotOnRelfilenodeDrop(450, (XLogRecPtr) 7000, 2);
	assert_false(AnchorSnapshotLookup("a", &xmin, NULL));
	assert_false(AnchorSnapshotLookup("c", &xmin, NULL));

	/* an empty registry: no barrier */
	AnchorSnapshotOnRelfilenodeDrop(600, (XLogRecPtr) 7100, 1);

	anchorRegistry = NULL;
	free(reg);
}

/*
 * The kept anchor (a file a start could not register) is removed by both
 * hooks once the record lies past its restore point and the horizon
 * reaches its xmin; it has no entry, so no barrier runs for it.  The clear
 * at the end of recovery forgets it too.
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

	keptSet = true;
	AnchorSnapshotOnRelfilenodeDrop(500, (XLogRecPtr) 1000, 1);
	assert_true(keptSet);
	AnchorSnapshotOnRelfilenodeDrop(500, (XLogRecPtr) 1001, 1);
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
 * it, and none of them but a file-dropping commit touches the drop
 * horizon.
 */
static void
test__removal_count_and_horizon(void **state)
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

	/* none of that moved the drop horizon */
	assert_int_equal(pg_atomic_read_u32(&reg->dropOrdinalHorizon), 0);

	anchorRegistry = NULL;
	free(reg);
	whpg_hot_standby_anchor_name = NULL;
}

/*
 * With nothing registered and no kept file the hooks return before the
 * scan.  An entry planted as valid while the count says zero survives a
 * horizon that would invalidate it, which only the early return explains;
 * the drop hook still raises the drop horizon, an ordinal having been
 * handed out on this node.
 */
static void
test__hooks_skip_an_empty_registry(void **state)
{
	AnchorRegistryData *reg = makeRegistry(4);
	RelFileNode node = {1663, 12345, 16384};
	TransactionId xmin;

	keptSet = false;
	assert_int_equal(registerAnchor("a", 400), REGISTER_OK);
	reg->nvalid = 0;			/* the plant: the scan would find "a" */

	AnchorSnapshotOnCleanupRecord(1000, node, (XLogRecPtr) 5000);
	assert_true(AnchorSnapshotLookup("a", &xmin, NULL));

	AnchorSnapshotOnRelfilenodeDrop(1000, (XLogRecPtr) 5000, 3);
	assert_true(AnchorSnapshotLookup("a", &xmin, NULL));
	assert_int_equal(pg_atomic_read_u32(&reg->dropOrdinalHorizon), 1);

	anchorRegistry = NULL;
	free(reg);
}

/*
 * The verdict a backend's lock-time check reaches for the ordinal its
 * active snapshot carries: none before any file-dropping commit; after
 * one, every ordinal handed out before it predates it (registered,
 * retired or evicted alike) and every later one does not; no anchor and a
 * disabled registry never do.
 */
static void
test__drop_horizon_verdicts(void **state)
{
	AnchorRegistryData *reg = makeRegistry(4);
	TransactionId xmin;
	uint32		oa,
				ob,
				oc,
				od;

	/* a node that never registered an anchor leaves the horizon alone */
	AnchorSnapshotOnRelfilenodeDrop(450, (XLogRecPtr) 5000, 1);
	assert_int_equal(pg_atomic_read_u32(&reg->dropOrdinalHorizon), 0);

	assert_int_equal(registerAnchor("a", 400), REGISTER_OK);
	assert_int_equal(registerAnchor("b", 410), REGISTER_OK);
	assert_true(AnchorSnapshotLookup("a", &xmin, &oa));
	assert_true(AnchorSnapshotLookup("b", &xmin, &ob));
	assert_false(anchorOrdinalPredatesDrop(0));
	assert_false(anchorOrdinalPredatesDrop(oa));

	/* publication of b retires a: running statements on a carry on */
	whpg_hot_standby_anchor_name = "b";
	expectBarrier(1);
	assert_true(applyPublication("b"));
	assert_false(anchorOrdinalPredatesDrop(oa));
	assert_false(anchorOrdinalPredatesDrop(ob));

	/* a file-dropping commit: a (retired, maybe in use) and b predate it */
	expectBarrier(1);
	AnchorSnapshotOnRelfilenodeDrop(500, (XLogRecPtr) 6000, 1);
	assert_true(anchorOrdinalPredatesDrop(oa));
	assert_true(anchorOrdinalPredatesDrop(ob));
	assert_false(anchorOrdinalPredatesDrop(0));

	/* an anchor registered afterwards does not, until the next drop */
	assert_int_equal(registerAnchor("c", 600), REGISTER_OK);
	assert_true(AnchorSnapshotLookup("c", &xmin, &oc));
	assert_false(anchorOrdinalPredatesDrop(oc));
	assert_int_equal(registerAnchor("d", 610), REGISTER_OK);
	assert_true(AnchorSnapshotLookup("d", &xmin, &od));

	/* an eviction moves nothing either: c evicted (d is published) */
	whpg_hot_standby_anchor_name = "d";
	expectBarrier(1);
	assert_true(evictOldestUnpublished("e"));
	assert_false(AnchorSnapshotLookup("c", &xmin, NULL));
	assert_false(anchorOrdinalPredatesDrop(oc));

	/* the next drop covers the evicted c and the registered d alike */
	expectBarrier(1);
	AnchorSnapshotOnRelfilenodeDrop(700, (XLogRecPtr) 7000, 1);
	assert_true(anchorOrdinalPredatesDrop(oc));
	assert_true(anchorOrdinalPredatesDrop(od));

	/* a disabled registry never reports one */
	reg->capacity = 0;
	assert_false(anchorOrdinalPredatesDrop(oc));

	anchorRegistry = NULL;
	free(reg);
	whpg_hot_standby_anchor_name = NULL;
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
		unit_test(test__list_in_registration_order),
		unit_test(test__retire_on_publish),
		unit_test(test__evict_oldest_unpublished),
		unit_test(test__file_roundtrip_and_validation),
		unit_test(test__parse_returns_xid_set),
		unit_test(test__oldest_xmin_and_restart_rule),
		unit_test(test__cleanup_record_invalidates_at_or_below),
		unit_test(test__relfilenode_drop_invalidates_all),
		unit_test(test__kept_anchor_gate),
		unit_test(test__removal_count_and_horizon),
		unit_test(test__hooks_skip_an_empty_registry),
		unit_test(test__drop_horizon_verdicts),
	};

	MemoryContextInit();

	return run_tests(tests);
}
