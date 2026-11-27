# Tests for gp_internal_tls: mutual TLS authentication of the internal
# connections between the nodes of a cluster, which carry the GPDB "internal
# connection" protocol marker.
#
#   disable   : the marker alone is honored (the legacy bypass)
#   verify-ca : a client certificate that verifies against the cluster CA
#               (gp_internal_tls_ca_file) is mandatory
#
# The cluster's trust domain and the external one (ssl_ca_file) are kept apart
# in both directions: a certificate of the external CA is no cluster
# credential, and a certificate of the cluster CA does not log a client
# application in.
#
# The certificates of src/test/ssl play these parts:
#   cluster CA       root+server_ca
#   node certificate server-cn-only (issued by the cluster CA); a node uses it
#                    as its server certificate and as its client certificate
#   server-revoked   a node certificate that root+server.crl revokes
#   external CA      root+client_ca
#   user certificate client (issued by the external CA, CN=ssltestuser); it
#                    also serves as the server certificate of a coordinator,
#                    which belongs to the external domain
#
# The internal connections are made over TCP (hostaddr): a connection over a
# Unix-domain socket is trusted in every mode.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use Cwd qw(getcwd);
use File::Copy;
use FindBin;
use lib $FindBin::RealBin;
use SSLServer;

if ($ENV{with_openssl} ne 'yes')
{
	plan skip_all => 'SSL not supported by this build';
}

my $SERVERHOSTADDR = '127.0.0.1';

# The client keys must not be world-readable.
copy("ssl/client.key", "ssl/client_tmp.key");
chmod 0600, "ssl/client_tmp.key";
copy("ssl/server-cn-only.key", "ssl/client-node_tmp.key");
chmod 0600, "ssl/client-node_tmp.key";
copy("ssl/server-revoked.key", "ssl/client-node-revoked_tmp.key");
chmod 0600, "ssl/client-node-revoked_tmp.key";

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

configure_test_server_for_ssl($node, $SERVERHOSTADDR, 'trust');

my $ssldir = getcwd() . "/ssl";

# Write the SSL and gp_internal_tls settings.  gp_internal_tls is
# PGC_POSTMASTER, and pg_hba.conf only lets SSL connections in, so everything
# goes to postgresql.conf, where the last setting wins.  The server certificates
# are in the data directory; the CA files are taken from ssl/.  No CRL unless
# asked for: it is needed for every chain then.
sub write_conf
{
	my (%p) = @_;
	my $ssl_ca = $p{ssl_ca} ? "$ssldir/$p{ssl_ca}.crt" : '';
	my $internal_ca =
	  defined $p{internal_ca}
	  ? ($p{internal_ca} ? "$ssldir/$p{internal_ca}.crt" : '')
	  : "$ssldir/root+server_ca.crt";
	my $server_cert   = $p{server_cert}   // 'server-cn-only';
	my $internal_cert = $p{internal_cert} // 'server-cn-only';
	my $internal_key  = $p{internal_key}  // $internal_cert;
	my $ssl           = $p{ssl}           // 'on';
	my $crl           = $p{crl} ? "$ssldir/$p{crl}.crl" : '';

	# Leading newline: configure_test_server_for_ssl() ends the file without one.
	$node->append_conf(
		'postgresql.conf', "
ssl = $ssl
ssl_cert_file = '$server_cert.crt'
ssl_key_file = '$server_cert.key'
ssl_ca_file = '$ssl_ca'
ssl_crl_file = '$crl'
gp_internal_tls_cert_file = '$internal_cert.crt'
gp_internal_tls_key_file = '$internal_key.key'
gp_internal_tls_ca_file = '$internal_ca'
gp_internal_tls = '$p{mode}'
");
}

sub apply_conf
{
	write_conf(@_);
	$node->restart;
}

# The server must refuse to start with these settings, and say why.
sub start_fails
{
	my ($test_name, $expected, %p) = @_;
	my $log_start = -s $node->logfile;

	write_conf(%p);
	is($node->start(fail_ok => 1), 0, "$test_name: the server does not start");
	like(slurp_file($node->logfile, $log_start),
		$expected, "$test_name: the reason is logged");
}

# The node of the test harness is a segment.  Start it as a coordinator instead.
sub start_as_coordinator
{
	system_or_bail(
		'pg_ctl', '-w', '-D', $node->data_dir, '-l', $node->logfile, '-o',
		"-c gp_role=utility --gp_dbid=$node->{_dbid} --gp_contentid=-1 -c maintenance_mode=on",
		'start');
}

sub stop_coordinator
{
	system_or_bail('pg_ctl', '-w', '-D', $node->data_dir, '-m', 'fast',
		'stop');
}

# sslmode=require encrypts and presents the client certificate without
# verifying the server's, so only the client certificate is under test.
my $node_cert =
  "sslrootcert=invalid sslcert=ssl/server-cn-only.crt sslkey=ssl/client-node_tmp.key";
my $user_cert =
  "sslrootcert=invalid sslcert=ssl/client.crt sslkey=ssl/client_tmp.key";
my $revoked_cert =
  "sslrootcert=invalid sslcert=ssl/server-revoked.crt sslkey=ssl/client-node-revoked_tmp.key";
my $no_cert = "sslrootcert=invalid sslcert=invalid sslkey=invalid";

# An internal connection carries the marker (gpconntype).
my $internal =
  "user=ssltestuser dbname=trustdb hostaddr=$SERVERHOSTADDR gpconntype=default";

# A client application, to databases with these pg_hba.conf lines:
#   trustdb   trust
#   verifydb  trust, clientcert=verify-ca for yetanotheruser
#   certdb    cert
my $app = "hostaddr=$SERVERHOSTADDR sslmode=require";

my $rejected = qr/a verified client certificate is required/;

### verify-ca on a segment with no ssl_ca_file, the usual setup of a segment

apply_conf(mode => 'verify-ca', ssl_ca => '');

$node->connect_ok(
	"$internal sslmode=require $node_cert",
	"verify-ca: an internal connection with a node certificate is accepted");

$node->connect_fails(
	"$internal sslmode=require $no_cert",
	"verify-ca: an internal connection without a certificate is rejected",
	expected_stderr => $rejected);

$node->connect_fails(
	"$internal sslmode=disable",
	"verify-ca: an internal connection without TLS is rejected",
	expected_stderr => $rejected);

# The certificate of another CA gets through the handshake, where only the
# cluster CA is loaded, but not through authentication; the log tells which
# certificate it was.
$node->connect_fails(
	"$internal sslmode=require $user_cert",
	"verify-ca: an internal connection with a certificate of another CA is rejected",
	expected_stderr => $rejected,
	log_like        => [
		qr/does not verify against the internal cluster CA/,
		qr/Failed certificate data \(unverified\): subject "\/CN=ssltestuser"/
	]);

# The cluster CA was loaded for the internal connections.  A client application
# must not notice: it gets in as before, with whatever certificate it has ...
$node->connect_ok(
	"user=ssltestuser dbname=trustdb $app $user_cert",
	"verify-ca, no ssl_ca_file: a client application with a certificate of another CA connects"
);

# ... but the cluster CA is no root certificate store for pg_hba.conf, so a
# node certificate does not log anyone in.
$node->connect_fails(
	"user=yetanotheruser dbname=verifydb $app $node_cert",
	"verify-ca, no ssl_ca_file: clientcert is not satisfied by a node certificate",
	expected_stderr => qr/root certificate store/);

$node->connect_fails(
	"user=ssltestuser dbname=certdb $app $node_cert",
	"verify-ca, no ssl_ca_file: cert authentication is not satisfied by a node certificate",
	expected_stderr => qr/root certificate store/);

### verify-ca with a CRL: ssl_crl_file applies to the node certificates too

apply_conf(mode => 'verify-ca', ssl_ca => '', crl => 'root+server');

$node->connect_ok(
	"$internal sslmode=require $node_cert",
	"CRL: an internal connection with a node certificate is accepted");

# Only the cluster CA is loaded, so the handshake lets the certificate through;
# authentication must not.
$node->connect_fails(
	"$internal sslmode=require $revoked_cert",
	"CRL: an internal connection with a revoked node certificate is rejected",
	expected_stderr => $rejected,
	log_like        => [qr/internal connection was rejected: certificate revoked/]);

apply_conf(
	mode   => 'verify-ca',
	ssl_ca => 'root+client_ca',
	crl    => 'root+server');

$node->connect_fails(
	"$internal sslmode=require $revoked_cert",
	"CRL, two CAs: a revoked node certificate is rejected in the handshake",
	expected_stderr => qr/SSL error/);

### verify-ca with an external ssl_ca_file: two trust domains on one server

apply_conf(mode => 'verify-ca', ssl_ca => 'root+client_ca');

$node->connect_ok(
	"$internal sslmode=require $node_cert",
	"two CAs: an internal connection is verified against the cluster CA, not ssl_ca_file"
);

$node->connect_fails(
	"$internal sslmode=require $user_cert",
	"two CAs: a certificate of the external CA is no cluster credential",
	expected_stderr => $rejected);

$node->connect_ok(
	"user=ssltestuser dbname=certdb $app $user_cert",
	"two CAs: a client application logs in with a certificate of the external CA"
);

$node->connect_fails(
	"user=yetanotheruser dbname=verifydb $app $node_cert",
	"two CAs: clientcert is not satisfied by a node certificate",
	expected_stderr => qr/connection requires a valid client certificate/);

$node->connect_fails(
	"user=ssltestuser dbname=certdb $app $node_cert",
	"two CAs: cert authentication is not satisfied by a node certificate",
	expected_stderr => qr/connection requires a valid client certificate/);

# The CAs swapped: ssl_ca_file alone must not make a certificate a cluster
# credential.  The cluster CA is root+client_ca here, which has issued no
# server certificate, so the node runs as a coordinator, whose server
# certificate need not be the cluster's.
$node->stop;
copy("ssl/client.crt", $node->data_dir . "/client.crt");
copy("ssl/client.key", $node->data_dir . "/client.key");
chmod 0600, $node->data_dir . "/client.key";
write_conf(
	mode          => 'verify-ca',
	ssl_ca        => 'root+server_ca',
	internal_ca   => 'root+client_ca',
	internal_cert => 'client');
start_as_coordinator();

$node->connect_fails(
	"$internal sslmode=require $node_cert",
	"CAs swapped: a certificate that only ssl_ca_file trusts is rejected",
	expected_stderr => $rejected);

$node->connect_ok(
	"$internal sslmode=require $user_cert",
	"CAs swapped: a certificate of the cluster CA is accepted");

stop_coordinator();

### verify-ca on a coordinator

# Its server certificate belongs to the external domain.
write_conf(mode => 'verify-ca', ssl_ca => '', server_cert => 'client');
start_as_coordinator();

# The connection to the entry DB comes over TCP when PGHOST is set.  It is from
# the same host, and still needs the certificate.
$node->connect_fails(
	"$internal sslmode=disable",
	"coordinator: an internal connection from the same host over TCP without a certificate is rejected",
	expected_stderr => $rejected);

$node->connect_ok(
	"$internal sslmode=require $node_cert",
	"coordinator: an internal connection from the same host over TCP with a node certificate is accepted"
);

# Over a Unix-domain socket libpq does not negotiate SSL, whatever sslmode
# says, and the connection is trusted.
$node->connect_ok(
	"dbname=postgres host="
	  . $node->host
	  . " gpconntype=default sslmode=require $node_cert",
	"coordinator: an internal connection over a Unix-domain socket is accepted"
);

stop_coordinator();

### verify-ca: settings the server refuses to start with

start_fails(
	"ssl off",
	qr/gp_internal_tls is set to "verify-ca" but "ssl" is off/,
	mode => 'verify-ca',
	ssl  => 'off');

start_fails(
	"no cluster CA",
	qr/the internal TLS certificate GUCs are not all set/,
	mode        => 'verify-ca',
	internal_ca => '');

start_fails(
	"client certificate of another CA",
	qr/certificate file "client.crt" set in "gp_internal_tls_cert_file" does not verify against the internal cluster CA/,
	mode          => 'verify-ca',
	internal_cert => 'client');

start_fails(
	"client key of another certificate",
	qr/private key file "server-revoked.key" set in "gp_internal_tls_key_file" does not match certificate file "server-cn-only.crt"/,
	mode         => 'verify-ca',
	internal_key => 'server-revoked');

start_fails(
	"segment with a server certificate of another CA",
	qr/certificate file "client.crt" set in "ssl_cert_file" does not verify against the internal cluster CA/,
	mode        => 'verify-ca',
	server_cert => 'client');

### disable: the marker alone is honored

write_conf(mode => 'disable', ssl_ca => 'root+client_ca');
$node->start;

$node->connect_ok(
	"$internal sslmode=disable",
	"disable: an internal connection is accepted without TLS or a certificate"
);

# The cluster CA is not loaded, so it is no trust anchor at all.
$node->connect_fails(
	"user=yetanotheruser dbname=verifydb $app $node_cert",
	"disable: a node certificate is rejected in the handshake",
	expected_stderr => qr/SSL error/);

done_testing();
