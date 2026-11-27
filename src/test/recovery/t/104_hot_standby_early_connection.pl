# Checks that a hot standby does not serve clients before hot standby is
# active.
#
# The standby restarts from a restartpoint taken while the primary has a
# transaction open whose subtransactions overflowed the running-xacts record.
# Hot standby then cannot become active until that transaction ends, although
# the walreceiver is already streaming.  Its transaction state is incomplete
# in that window, so a snapshot taken from it can judge the open transaction's
# rows as aborted and hint them so, hiding them for good once it commits.  A
# connection must therefore be refused until hot standby is active, and after
# the commit the standby must see every row.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 4;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

$node_primary->safe_psql('postgres',
	"CREATE TABLE t (a int, b int);
	 INSERT INTO t SELECT i, 0 FROM generate_series(1, 30) i;
	 CREATE TABLE t2 (a int);");

my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
# With gp_disable_tuple_hints (the default) such hints are not written out;
# keep the pages in shared buffers so that a hint set too early would last
# until the rows are counted at the end.
$node_standby->append_conf('postgresql.conf', 'shared_buffers = 128MB');
$node_standby->start;
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));

# A transaction with more subtransactions than PGPROC_MAX_CACHED_SUBXIDS (64)
# inserts 2100 rows and stays open.
my $psql_primary = $node_primary->background_psql('postgres');
$psql_primary->query_safe(
	"BEGIN;
	 DO \$\$
	 BEGIN
	   FOR i IN 1..70 LOOP
	     BEGIN
	       INSERT INTO t SELECT g, i FROM generate_series(1, 30) g;
	     EXCEPTION WHEN others THEN RAISE;
	     END;
	   END LOOP;
	 END \$\$;");

# A later transaction commits, so that a snapshot's upper bound lies above
# the open transaction's subtransactions.
$node_primary->safe_psql('postgres',
	"INSERT INTO t2 SELECT generate_series(1, 30)");

# The standby makes a restartpoint at a checkpoint whose running-xacts
# record is overflowed, and restarts from it.
$node_primary->safe_psql('postgres', "CHECKPOINT");
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));
$node_standby->safe_psql('postgres', "CHECKPOINT");
$node_standby->restart;

my ($ret, $stdout, $stderr) =
  $node_standby->psql('postgres', "SELECT count(*) FROM t");
isnt($ret, 0, "connection refused before hot standby is active");
like(
	$stderr,
	qr/the database system is in recovery mode/,
	"refusal says the database system is in recovery mode");

# Commit, and let the standby become active.  The checkpoint logs a
# running-xacts record without the open transaction.
$psql_primary->query_safe("COMMIT");
$psql_primary->quit;
$node_primary->safe_psql('postgres', "CHECKPOINT");
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));
ok( $node_standby->poll_query_until(
		'postgres', "SELECT pg_is_in_recovery()", 't'),
	"standby accepts connections once hot standby is active");

is($node_standby->safe_psql('postgres', "SELECT count(*) FROM t"),
	'2130', "standby sees every row committed on the primary");
