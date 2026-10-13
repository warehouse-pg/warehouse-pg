#!/usr/bin/env bash
# Log assertions for the hot_standby anchor suites, against the csv server
# logs of the standby nodes (role 'm' in gp_segment_configuration).
#
#   hs_anchor_log.sh mark <tag> <content>...
#       remember the current end of each node's log under <tag>
#   hs_anchor_log.sh grep <tag> <content> <ERE>
#       succeed once a line logged since the mark matches <ERE>
#       (polled for up to five seconds)
#   hs_anchor_log.sh absent <tag> <content> <ERE>
#       succeed when no line logged since the mark matches <ERE>
#   hs_anchor_log.sh active <tag> <content> <LIKE pattern>
#       succeed once a backend on that standby node runs a query matching
#       the pattern (polled for up to ten seconds); <tag> is unused
#
# The csv logs double every quote of a message, so match quotes with '..',
# and carry the SQLSTATE in their own column: '"ERROR","55000","anchor ...'.
# An error an executor raised carries the segment suffix in the
# coordinator's log; one the coordinator raised does not.
set -u
cmd=$1; tag=$2; shift 2

datadir() {
	psql -d postgres -Atc "select datadir from gp_segment_configuration where content = $1 and role = 'm'"
}

offfile() {
	echo "/tmp/hs_anchor_log_${PGPORT}_${tag}_$1"
}

since() {
	local d o
	d=$(datadir "$1")
	[ -n "$d" ] || return 1
	o=$(cat "$(offfile "$1")" 2>/dev/null)
	[ -n "$o" ] || o=999999999999
	cat "$d"/log/*.csv | tail -c +$((o + 1))
}

case "$cmd" in
mark)
	for c in "$@"; do
		d=$(datadir "$c")
		[ -n "$d" ] || exit 1
		cat "$d"/log/*.csv | wc -c > "$(offfile "$c")"
	done
	;;
grep)
	for i in $(seq 1 50); do
		since "$1" | grep -qE -- "$2" && exit 0
		sleep 0.1
	done
	exit 1
	;;
absent)
	! since "$1" | grep -qE -- "$2"
	;;
active)
	port=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = $1 and role = 'm'")
	[ -n "$port" ] || exit 1
	for i in $(seq 1 100); do
		n=$(PGOPTIONS='-c gp_role=utility' psql -X -p "$port" -d postgres -Atc "select count(*) from pg_stat_activity where state = 'active' and pid <> pg_backend_pid() and query like '$2'")
		[ "${n:-0}" -ge 1 ] && exit 0
		sleep 0.1
	done
	exit 1
	;;
*)
	exit 2
	;;
esac
