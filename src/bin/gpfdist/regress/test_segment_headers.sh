#!/bin/bash
#
# Regression test for the X-GP-SEGMENT-COUNT / X-GP-SEGMENT-ID bounds checks
# in session_attach().  An out-of-range or negative segment id must be
# rejected with HTTP 400 instead of indexing past the per-segment arrays,
# while a plain download that omits the segment headers must still succeed.

PORT=8088
DATADIR=$(mktemp -d)
GPFDIST_PID=0
rc=0

function cleanup()
{
	if [ $GPFDIST_PID -ne 0 ]; then
		kill $GPFDIST_PID >/dev/null 2>&1
		GPFDIST_PID=0
	fi
	rm -rf "$DATADIR"
}
trap cleanup EXIT

if ! which curl >/dev/null 2>&1; then
	echo -e "\033[32m curl not available, skipping segment-header test\033[0m"
	exit 0
fi

printf 'row1\nrow2\n' > "$DATADIR/data.txt"

gpfdist -d "$DATADIR" -p $PORT >/dev/null 2>&1 &
GPFDIST_PID=$!
sleep 2

if ! kill -0 $GPFDIST_PID >/dev/null 2>&1; then
	echo -e "\033[31m gpfdist failed to start on port $PORT\033[0m"
	exit 1
fi

# $1 = description, $2 = expected HTTP status, rest = curl header args
function check()
{
	local desc="$1"; local want="$2"; shift 2
	local got
	got=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/data.txt" "$@")
	if [ "$got" == "$want" ]; then
		echo -e "\033[32m ok   [$desc] -> $got\033[0m"
	else
		echo -e "\033[31m FAIL [$desc] expected $want, got $got\033[0m"
		rc=1
	fi
}

PROTO="-H X-GP-PROTO:1"

# a download that omits the segment headers is served as a single consumer
check "proto only, no segment headers" 200 $PROTO
# a well-formed single-segment request succeeds
check "count=1 id=0" 200 $PROTO -H X-GP-SEGMENT-COUNT:1 -H X-GP-SEGMENT-ID:0
# an id past the segment count is rejected
check "count=1 id=2000000000" 400 $PROTO -H X-GP-SEGMENT-COUNT:1 -H X-GP-SEGMENT-ID:2000000000
# a negative id is rejected
check "count=4 id=-1" 400 $PROTO -H X-GP-SEGMENT-COUNT:4 -H X-GP-SEGMENT-ID:-1
# a non-positive count is rejected
check "count=0 id=0" 400 $PROTO -H X-GP-SEGMENT-COUNT:0 -H X-GP-SEGMENT-ID:0
# a count larger than any cluster can have is rejected
check "count=2000000000 id=0" 400 $PROTO -H X-GP-SEGMENT-COUNT:2000000000 -H X-GP-SEGMENT-ID:0

# the server must still be serving after the rejected requests
check "normal request after rejects" 200 $PROTO -H X-GP-SEGMENT-COUNT:1 -H X-GP-SEGMENT-ID:0

if ! kill -0 $GPFDIST_PID >/dev/null 2>&1; then
	echo -e "\033[31m gpfdist is no longer running after the bad requests\033[0m"
	rc=1
fi

if [ $rc -eq 0 ]; then
	echo -e "\033[32m test [gpfdist segment header bounds] success\033[0m"
else
	echo -e "\033[31m test [gpfdist segment header bounds] failed\033[0m"
fi
exit $rc
