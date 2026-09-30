#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "utils/memutils.h"

/* Actual function body */
#include "../cdbdtxcontextinfo.c"

static MemoryContext exception_cxt;

/* the option bit and helpers live in cdbtm.c, linked in */
#define TEST_TXN_OPTIONS	(0x0004 | 0x0010)	/* READ COMMITTED, read only */

/*
 * A zeroed context whose distributed xid array is already allocated:
 * DistributedSnapshot_Reset() would otherwise size one through the
 * procarray, which a unit test does not have.
 */
static void
initContext(DtxContextInfo *info)
{
	memset(info, 0, sizeof(*info));
	info->distributedSnapshot.inProgressXidArray =
		(DistributedTransactionId *) malloc(16 * sizeof(DistributedTransactionId));
}

/*
 * A context with no distributed xid and no distributed snapshot: the shape
 * of every read dispatched by a hot-standby coordinator.
 */
static void
fillPlainContext(DtxContextInfo *info)
{
	initContext(info);
	DtxContextInfo_Reset(info);
	info->distributedXid = InvalidDistributedTransactionId;
	info->segmateSync = 7;
	info->nestingLevel = 1;
	info->haveDistributedSnapshot = false;
	info->cursorContext = false;
	info->distributedTxnOptions = TEST_TXN_OPTIONS;
	info->curcid = 3;
}

/*
 * The wire layout of the plain context, byte for byte, as it was before the
 * anchor name existed: xid(8) segmateSync(4) nestingLevel(4) two bools
 * options(4).
 */
static int
expectedPlainBytes(char *buf, int options)
{
	char	   *p = buf;
	DistributedTransactionId xid = InvalidDistributedTransactionId;
	uint32		segmateSync = 7;
	uint32		nestingLevel = 1;
	bool		f = false;

	memcpy(p, &xid, sizeof(xid)); p += sizeof(xid);
	memcpy(p, &segmateSync, sizeof(uint32)); p += sizeof(uint32);
	memcpy(p, &nestingLevel, sizeof(uint32)); p += sizeof(uint32);
	memcpy(p, &f, sizeof(bool)); p += sizeof(bool);
	memcpy(p, &f, sizeof(bool)); p += sizeof(bool);
	memcpy(p, &options, sizeof(int)); p += sizeof(int);
	return p - buf;
}

/*
 * Without the anchor bit the serialized context is unchanged: same size,
 * same bytes, and a name left in the struct is neither written nor read.
 */
static void
test__serialize_without_anchor_is_unchanged(void **state)
{
	DtxContextInfo info;
	DtxContextInfo back;
	char		expected[128];
	char		buf[128];
	int			explen;
	int			size;

	fillPlainContext(&info);
	strlcpy(info.anchorName, "stale name, must not travel", MAXFNAMELEN);

	explen = expectedPlainBytes(expected, TEST_TXN_OPTIONS);
	size = DtxContextInfo_SerializeSize(&info);
	assert_int_equal(size, explen);
	assert_int_equal(size, 22);

	memset(buf, 0xAB, sizeof(buf));
	DtxContextInfo_Serialize(buf, &info);
	assert_memory_equal(buf, expected, explen);
	/* nothing written past the announced size */
	assert_true((unsigned char) buf[explen] == 0xAB);

	initContext(&back);
	DtxContextInfo_Deserialize(buf, size, &back);
	assert_int_equal(back.distributedTxnOptions, TEST_TXN_OPTIONS);
	assert_false(isMppTxOptions_Anchored(back.distributedTxnOptions));
	assert_string_equal(back.anchorName, "");
	assert_int_equal(back.segmateSync, 7);
	assert_int_equal(back.nestingLevel, 1);
}

/*
 * With the bit set the name follows the options, MAXFNAMELEN bytes, and
 * comes back on the other side; the bytes before it are the plain layout
 * with the bit in the options.
 */
static void
test__serialize_with_anchor_round_trip(void **state)
{
	DtxContextInfo info;
	DtxContextInfo back;
	char		expected[128];
	char		buf[128];
	int			explen;
	int			size;
	int			options = mppTxOptions_SetAnchored(TEST_TXN_OPTIONS);

	fillPlainContext(&info);
	info.distributedTxnOptions = options;
	strlcpy(info.anchorName, "rp_20260924T180000", MAXFNAMELEN);

	assert_true(isMppTxOptions_Anchored(options));
	assert_int_equal(options & TEST_TXN_OPTIONS, TEST_TXN_OPTIONS);

	explen = expectedPlainBytes(expected, options);
	size = DtxContextInfo_SerializeSize(&info);
	assert_int_equal(size, explen + MAXFNAMELEN);

	memset(buf, 0xAB, sizeof(buf));
	DtxContextInfo_Serialize(buf, &info);
	assert_memory_equal(buf, expected, explen);
	assert_string_equal(buf + explen, "rp_20260924T180000");
	assert_true((unsigned char) buf[size] == 0xAB);

	initContext(&back);
	DtxContextInfo_Deserialize(buf, size, &back);
	assert_true(isMppTxOptions_Anchored(back.distributedTxnOptions));
	assert_string_equal(back.anchorName, "rp_20260924T180000");
	assert_int_equal(back.segmateSync, 7);
}

/*
 * Copy carries the name, Reset clears it.
 */
static void
test__copy_and_reset_name(void **state)
{
	DtxContextInfo info;
	DtxContextInfo copy;

	fillPlainContext(&info);
	info.distributedTxnOptions = mppTxOptions_SetAnchored(TEST_TXN_OPTIONS);
	strlcpy(info.anchorName, "rp_copy", MAXFNAMELEN);

	initContext(&copy);
	DtxContextInfo_Copy(&copy, &info);
	assert_string_equal(copy.anchorName, "rp_copy");
	assert_true(isMppTxOptions_Anchored(copy.distributedTxnOptions));

	DtxContextInfo_Reset(&copy);
	assert_string_equal(copy.anchorName, "");
	assert_int_equal(copy.distributedTxnOptions, 0);
}

static void
expectDeserializeError(const char *buf, int len)
{
	DtxContextInfo back;
	bool		caught = false;

	initContext(&back);
	PG_TRY();
	{
		DtxContextInfo_Deserialize(buf, len, &back);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(exception_cxt);
		edata = CopyErrorData();
		FlushErrorState();
		assert_int_equal(edata->sqlerrcode, ERRCODE_PROTOCOL_VIOLATION);
		assert_int_equal(edata->elevel, ERROR);
		caught = true;
	}
	PG_END_TRY();
	assert_true(caught);
}

/*
 * A context that announces a name but is too short to hold one, or holds
 * a name that is not a valid anchor name, is refused.
 */
static void
test__deserialize_refuses_bad_anchor_field(void **state)
{
	DtxContextInfo info;
	char		buf[128];
	int			size;
	int			options = mppTxOptions_SetAnchored(TEST_TXN_OPTIONS);

	fillPlainContext(&info);
	info.distributedTxnOptions = options;
	strlcpy(info.anchorName, "rp_ok", MAXFNAMELEN);
	size = DtxContextInfo_SerializeSize(&info);
	DtxContextInfo_Serialize(buf, &info);

	/* announced but truncated: only the plain 22 bytes are there */
	expectDeserializeError(buf, size - MAXFNAMELEN);

	/* a name that could escape the anchor directory */
	strlcpy(info.anchorName, "../escape", MAXFNAMELEN);
	DtxContextInfo_Serialize(buf, &info);
	expectDeserializeError(buf, size);

	/* an empty name */
	info.anchorName[0] = '\0';
	DtxContextInfo_Serialize(buf, &info);
	expectDeserializeError(buf, size);
}

int
main(int argc, char *argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
		unit_test(test__serialize_without_anchor_is_unchanged),
		unit_test(test__serialize_with_anchor_round_trip),
		unit_test(test__copy_and_reset_name),
		unit_test(test__deserialize_refuses_bad_anchor_field)
	};

	MemoryContextInit();
	exception_cxt = AllocSetContextCreate(TopMemoryContext,
										  "mock error handling context",
										  ALLOCSET_DEFAULT_MINSIZE,
										  ALLOCSET_DEFAULT_INITSIZE,
										  ALLOCSET_DEFAULT_MAXSIZE);

	return run_tests(tests);
}
