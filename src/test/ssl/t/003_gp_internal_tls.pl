# Tests for gp_internal_tls: mutual TLS authentication of internal
# (QD-to-QE) connections, which carry the GPDB "internal connection"
# protocol marker.  Verifies that:
#   - disable   : the marker alone is honored (legacy bypass)
#   - verify-ca : a client certificate that verifies against the *internal*
#                 cluster CA (gp_internal_tls_ca_file) is mandatory; anything
#                 else is rejected
#
# Crucially, the internal trust domain is exercised as *distinct* from the
# external ssl_ca_file domain: the server must verify internal peers against
# gp_internal_tls_ca_file, not ssl_ca_file.  The two cross-CA cases below fail
# if the server ever falls back to ssl_ca_file for internal authentication.
#
# Connections are made over TCP (hostaddr) because Unix-domain socket
# connections are always trusted locally and exempt from the TLS requirement.
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

# The client key must not be world-readable.
copy("ssl/client.key", "ssl/client_tmp.key");
chmod 0600, "ssl/client_tmp.key";

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

configure_test_server_for_ssl($node, $SERVERHOSTADDR, 'trust');

# Apply the server's SSL identity and the internal-TLS policy, then restart.
# gp_internal_tls is PGC_POSTMASTER, so everything is written to postgresql.conf
# (which is last-wins, so re-appending overrides the previous value) rather than
# set via SQL: configure_test_server_for_ssl() locks pg_hba.conf down to
# SSL-only, so a default local psql connection would be refused.
#
# The server always presents server-cn-only (signed by the server CA) as its own
# certificate.  $ssl_ca and $internal_ca are set independently so we can prove
# that internal authentication uses gp_internal_tls_ca_file, not ssl_ca_file.
# ssl_crl_file is cleared: a CRL applies to the whole verify store and would
# require CRLs for every chain, which the cross-CA cases do not provide.
sub apply_conf
{
	my ($ssl_ca, $internal_ca, $mode) = @_;
	# The server cert/key are staged in the data directory by
	# configure_test_server_for_ssl(); the CA files are referenced by absolute
	# path from the source ssl/ directory because only server-*.crt and root*
	# files are copied into the data directory.
	my $ssldir = getcwd() . "/ssl";
	# Leading newline: configure_test_server_for_ssl() writes its trailing
	# "include 'sslconfig.conf'" without a newline.
	my $conf = "\nssl = on"
		. "\nssl_cert_file = 'server-cn-only.crt'"
		. "\nssl_key_file = 'server-cn-only.key'"
		. "\nssl_ca_file = '$ssldir/$ssl_ca.crt'"
		. "\nssl_crl_file = ''"
		. "\ngp_internal_tls_cert_file = 'server-cn-only.crt'"
		. "\ngp_internal_tls_key_file = 'server-cn-only.key'"
		. "\ngp_internal_tls_ca_file = '$ssldir/$internal_ca.crt'"
		. "\ngp_internal_tls = '$mode'";
	$node->append_conf('postgresql.conf', $conf);
	$node->restart;
}

# Base connstr that carries the internal-connection marker via gpconntype=default.
# sslmode=require encrypts and presents the client certificate without verifying
# the server cert, isolating the client-cert (mutual-auth) check.
my $nocert   = "sslkey=invalid sslcert=invalid sslrootcert=invalid sslcrl=invalid";
my $with_cert = "sslrootcert=invalid sslcert=ssl/client.crt sslkey=ssl/client_tmp.key";
my $base = "user=ssltestuser dbname=trustdb hostaddr=$SERVERHOSTADDR gpconntype=default";

# client.crt is signed by the client CA (present in root+client_ca), NOT by the
# server CA (server_ca).  We use root+client_ca as the "cluster" CA and server_ca
# as the "external" CA so the two domains are genuinely disjoint for client.crt.

### verify-ca, single CA: baseline
apply_conf('root+client_ca', 'root+client_ca', 'verify-ca');

$node->connect_ok(
	"$base sslmode=require $with_cert",
	"verify-ca: internal connection with a verified client certificate is accepted");

$node->connect_fails(
	"$base sslmode=require $nocert",
	"verify-ca: internal connection without a client certificate is rejected",
	expected_stderr => qr/a verified client certificate is required/);

$node->connect_fails(
	"$base sslmode=disable",
	"verify-ca: plaintext internal connection is rejected",
	expected_stderr => qr/a verified client certificate is required/);

### verify-ca, two CAs: the internal CA is the trust anchor, not ssl_ca_file.
# ssl_ca_file = server_ca does NOT verify client.crt; gp_internal_tls_ca_file =
# root+client_ca does.  The connection must still succeed, which is only possible
# if the server loads and consults gp_internal_tls_ca_file.  (Regression guard
# for the bug where the server verified internal peers against ssl_ca_file only.)
apply_conf('server_ca', 'root+client_ca', 'verify-ca');

$node->connect_ok(
	"$base sslmode=require $with_cert",
	"verify-ca: client cert verified against the internal CA even when ssl_ca_file is a different CA");

### verify-ca, two CAs swapped: a cert trusted only by ssl_ca_file is rejected.
# ssl_ca_file = root+client_ca verifies client.crt (so the TLS handshake
# succeeds), but gp_internal_tls_ca_file = server_ca does not.  Internal auth must
# re-verify against the internal CA and reject it, keeping the trust domains
# separate (an external-CA leaf must not be a cluster credential).
apply_conf('root+client_ca', 'server_ca', 'verify-ca');

$node->connect_fails(
	"$base sslmode=require $with_cert",
	"verify-ca: a cert trusted only by ssl_ca_file is rejected for internal auth",
	expected_stderr => qr/a verified client certificate is required/);

### disable: legacy bypass, marker alone is honored
apply_conf('root+client_ca', 'root+client_ca', 'disable');

$node->connect_ok(
	"$base sslmode=disable",
	"disable: legacy internal connection is accepted without TLS");

done_testing();
