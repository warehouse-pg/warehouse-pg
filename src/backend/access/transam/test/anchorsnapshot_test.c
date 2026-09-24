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
	anchorRegistry = reg;
	return reg;
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

	assert_false(AnchorSnapshotLookup("rp1", &xmin));
	assert_false(registryFull());

	assert_true(registerAnchor("rp1", 100));
	assert_true(AnchorSnapshotLookup("rp1", &xmin));
	assert_int_equal(xmin, 100);

	/* a duplicate is refused and the existing entry kept */
	assert_false(registerAnchor("rp1", 200));
	assert_true(AnchorSnapshotLookup("rp1", &xmin));
	assert_int_equal(xmin, 100);

	assert_true(registerAnchor("rp2", 110));
	assert_true(registryFull());
	assert_false(registerAnchor("rp3", 120));
	assert_false(AnchorSnapshotLookup("rp3", &xmin));

	/* invalidation frees the slot on the spot */
	AnchorSnapshotInvalidate("rp1");
	assert_false(AnchorSnapshotLookup("rp1", &xmin));
	assert_false(registryFull());
	assert_true(registerAnchor("rp3", 120));
	assert_true(AnchorSnapshotLookup("rp3", &xmin));
	assert_int_equal(xmin, 120);

	free(anchorRegistry);
	anchorRegistry = NULL;
}

static void
test__registry_disabled(void **state)
{
	TransactionId xmin;
	AnchorSnapshotInfo info[1];

	makeRegistry(0);
	assert_false(registryEnabled());
	assert_false(AnchorSnapshotLookup("rp1", &xmin));
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
	assert_true(registerAnchor("c", 30));
	assert_true(registerAnchor("a", 10));
	assert_true(registerAnchor("b", 20));
	/* free the first slot and reuse it: the ordinal, not the slot, orders */
	AnchorSnapshotInvalidate("c");
	assert_true(registerAnchor("d", 40));

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
	assert_true(registerAnchor("rp1", 10));
	assert_true(registerAnchor("rp2", 20));
	assert_true(registerAnchor("rp3", 30));
	assert_true(registerAnchor("rp4", 40));

	/* the boot value is recorded by AnchorSnapshotStartup; emulate it */
	strlcpy(lastSeenAnchorName, "", MAXFNAMELEN);
	lastSeenAnchorNameSet = true;

	/* unknown name: nothing retired */
	whpg_hot_standby_anchor_name = "nope";
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp1", &xmin));
	assert_true(AnchorSnapshotLookup("rp4", &xmin));

	/* publish rp3: rp1 and rp2 go, rp3 and the newer rp4 stay */
	whpg_hot_standby_anchor_name = "rp3";
	AnchorSnapshotOnConfigReload();
	assert_false(AnchorSnapshotLookup("rp1", &xmin));
	assert_false(AnchorSnapshotLookup("rp2", &xmin));
	assert_true(AnchorSnapshotLookup("rp3", &xmin));
	assert_true(AnchorSnapshotLookup("rp4", &xmin));

	/* the same value again is not a publication */
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp3", &xmin));

	/* an empty name retires nothing either */
	whpg_hot_standby_anchor_name = "";
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp3", &xmin));
	assert_true(AnchorSnapshotLookup("rp4", &xmin));

	/* before Startup ran, a reload is ignored */
	lastSeenAnchorNameSet = false;
	whpg_hot_standby_anchor_name = "rp4";
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp3", &xmin));

	/*
	 * A reload naming an anchor not registered yet is completed by the
	 * export of that name: applyPublication is what the export calls.
	 */
	lastSeenAnchorNameSet = true;
	whpg_hot_standby_anchor_name = "rp5";
	AnchorSnapshotOnConfigReload();
	assert_true(AnchorSnapshotLookup("rp3", &xmin));
	assert_true(registerAnchor("rp5", 50));
	assert_true(applyPublication("rp5"));
	assert_false(AnchorSnapshotLookup("rp3", &xmin));
	assert_false(AnchorSnapshotLookup("rp4", &xmin));
	assert_true(AnchorSnapshotLookup("rp5", &xmin));
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
	assert_true(registerAnchor("a", 10));
	assert_true(registerAnchor("b", 20));
	assert_true(registerAnchor("c", 30));
	assert_true(registryFull());

	/* b is published: a (oldest) goes first, then c, never b */
	whpg_hot_standby_anchor_name = "b";
	assert_true(evictOldestUnpublished("d"));
	assert_false(AnchorSnapshotLookup("a", &xmin));
	assert_true(AnchorSnapshotLookup("b", &xmin));
	assert_true(AnchorSnapshotLookup("c", &xmin));
	assert_true(registerAnchor("d", 40));
	assert_true(registryFull());

	assert_true(evictOldestUnpublished("e"));
	assert_false(AnchorSnapshotLookup("c", &xmin));
	assert_true(AnchorSnapshotLookup("b", &xmin));
	assert_true(registerAnchor("e", 50));

	/* only the published anchor left: nothing to evict */
	assert_true(evictOldestUnpublished("f"));
	assert_false(AnchorSnapshotLookup("d", &xmin));
	assert_true(evictOldestUnpublished("f"));
	assert_false(AnchorSnapshotLookup("e", &xmin));
	assert_false(evictOldestUnpublished("f"));
	assert_true(AnchorSnapshotLookup("b", &xmin));

	/* no published anchor at all: plain oldest-first */
	whpg_hot_standby_anchor_name = "";
	assert_true(evictOldestUnpublished("f"));
	assert_false(AnchorSnapshotLookup("b", &xmin));

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
	assert_false(parseAnchorFile(name, &tli, &lsn, &xmin));
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
	assert_true(parseAnchorFile("rp1", &tli, &lsn, &xmin));
	assert_int_equal(tli, 2);
	assert_true(lsn == (XLogRecPtr) 0x16B3D50);
	assert_int_equal(xmin, 101);

	/* the grammar, line by line */
	anchorFilePath(path, sizeof(path), "rp1", false);
	f = fopen(path, "r");
	assert_true(f != NULL);
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_string_equal(line, "fmt:1\n");
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

	/* an overflowed snapshot carries no xid list */
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
	assert_string_equal(line, "rec:1\n");
	assert_true(fgets(line, sizeof(line), f) != NULL);
	assert_int_equal(strncmp(line, "crc:", 4), 0);
	fclose(f);
	assert_true(parseAnchorFile("rp2", &tli, &lsn, &xmin));
	assert_int_equal(xmin, 200);

	/* a file whose name line names another anchor is refused */
	anchorFilePath(path, sizeof(path), "rp3", false);
	assert_int_equal(rename("pg_anchor_snapshots/rp2", path), 0);
	assert_false(parseAnchorFile("rp3", &tli, &lsn, &xmin));

	/* the exact text again is accepted (crc intact) */
	expectRefused("rp1", "");
	{
		char	   *p;

		anchorFilePath(path, sizeof(path), "rp1", false);
		f = fopen(path, "w");
		assert_true(fputs(good, f) >= 0);
		fclose(f);
		assert_true(parseAnchorFile("rp1", &tli, &lsn, &xmin));

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
	assert_false(parseAnchorFile("rp9", &tli, &lsn, &xmin));

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

int
main(int argc, char *argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
		unit_test(test__anchorNameIsValid),
		unit_test(test__registry_register_lookup_full_duplicate),
		unit_test(test__registry_disabled),
		unit_test(test__list_in_registration_order),
		unit_test(test__retire_on_publish),
		unit_test(test__evict_oldest_unpublished),
		unit_test(test__file_roundtrip_and_validation),
	};

	MemoryContextInit();

	return run_tests(tests);
}
