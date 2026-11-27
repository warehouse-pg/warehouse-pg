# gp_internal_tls: mutual TLS on the internal connections between the nodes

The nodes of a cluster connect to each other with libpq: the coordinator
dispatches queries to the segments, the FTS probes them, and gp_inject_fault
reaches other nodes. These connections set a bit in the protocol version
(`GPDB_INTERNAL_PROTOCOL`), and a node that sees the bit skips authentication
(`FakeClientAuthentication()`). On a segment that is unconditional, so whoever
can reach a segment's port gets superuser access by setting the bit.

With `gp_internal_tls = verify-ca` the bit only tells the connection type. The
connecting node presents a client certificate, and the accepting node trusts
the connection only if that certificate verifies against the cluster CA. The
default, `disable`, keeps the old behavior, including libpq's default sslmode
for these connections.

The interconnect is not covered, and neither are replication connections,
which go through `pg_hba.conf`.

## Two trust domains

|Domain|GUCs|Used for|
|------|----|--------|
|External|`ssl_cert_file`, `ssl_key_file`, `ssl_ca_file`|client applications|
|Cluster|`gp_internal_tls_cert_file`, `gp_internal_tls_key_file`, `gp_internal_tls_ca_file`|connections between the nodes|

There is no fallback from the cluster files to the `ssl_*` ones, so that a
coordinator cannot dial the segments with its external certificate.

The two CA files must not have a root certificate in common. A certificate is
verified up to a root, so with a shared root in both files a certificate of
either domain verifies in the other.

A node is a TLS server with its `ssl_*` certificate and a TLS client with its
cluster certificate. A segment only serves internal connections, so its server
certificate is issued by the cluster CA and the coordinator verifies it
(`sslmode=verify-ca`; the nodes dial each other by address, so not
`verify-full`). The server certificate of the coordinator and of its standby
belongs to the external domain, so a node that connects to them does not verify
it (`sslmode=require`) but presents its cluster certificate all the same.

## Keeping the domains apart

The accepting node learns the connection type from the startup packet, which
comes after the TLS handshake. It cannot pick a CA for the handshake, so with
`verify-ca` both CAs are in the verify store of the SSL context. That alone
would make each CA a trust anchor for the other domain. Each CA is therefore
also loaded into a store of its own, and after the handshake the peer
certificate is verified against the store of its domain:

- An internal connection: against the cluster CA. A certificate issued by
  `ssl_ca_file` is no cluster credential.
- Any other connection: against `ssl_ca_file`. A certificate that does not
  verify, and any certificate when `ssl_ca_file` is not set, is dropped as if
  the client had sent none, so `cert` authentication and `clientcert` in
  `pg_hba.conf` never accept a certificate of the cluster CA.

When `ssl_ca_file` is not set, the cluster CA is the only reason the server asks
for a client certificate at all. A certificate that fails verification does not
fail the handshake then: without `gp_internal_tls` the server would not have
asked a client application for one, and that client must still get in. The
certificate is not trusted; an internal connection that sent it is rejected at
authentication.

`ssl_crl_file` is the one thing the domains share. The handshake applies it to
every client certificate, so it revokes node certificates too, and the check is
kept when the handshake is not failed for it. Since a CRL is then required for
every CA in a chain, a server that has both `ssl_ca_file` and `ssl_crl_file`
set needs the CRLs of the cluster CA in that file as well, or the internal
connections to it fail in the handshake.

## Startup checks

With `verify-ca` the postmaster does not start unless

- `ssl` is on and the three cluster files are set;
- the cluster certificate verifies against the cluster CA;
- libpq can use the cluster key: its file permissions are those `ssl_key_file`
  needs, it has no passphrase, and it matches the cluster certificate;
- on a segment, the server certificate verifies against the cluster CA too.

Otherwise the first sign of a wrong file would be a rejected connection, and
that is not harmless: the FTS takes a primary it cannot connect to for a failed
one. A coordinator with an unusable cluster certificate would find every
primary down.

The checks are made when the files are loaded, at startup and at a reload. They
do not help with a certificate that expires while the cluster runs: from then
on the connections with it are rejected, with the same consequence. The node
certificates have to be replaced before they expire. A reload (`gpstop -u`) is
enough for new files under the same names: the server certificate is loaded
again, and the client certificate is read for every new connection.

## Connections over a Unix-domain socket

libpq does not negotiate SSL over a Unix-domain socket, so there is no
certificate to verify. An internal connection over one is trusted in every
mode, on every node, before `pg_hba.conf` is consulted. The coordinator reaches
its own entry DB this way (over TCP, with the certificate, if `PGHOST` or
`PGHOSTADDR` is set in its environment).

So `gp_internal_tls` protects against a peer on the network, not against the
local users of a host: any process that can open a node's socket file can
connect to it as the bootstrap superuser by setting the bit. Access to the
socket file is the protection, see `unix_socket_permissions` (`0777` by
default) and `unix_socket_group`.

## Where the code is

- `internal_client_authentication()` in auth.c decides whether an internal
  connection is trusted.
- be-secure-openssl.c loads the CAs and checks the node's own certificates
  (`be_tls_init()`), and verifies a peer certificate against the CA of its
  domain (`be_tls_verify_peer_against_internal_ca()`,
  `be_tls_restrict_peer_to_external_ca()`).
- `cdbconn_add_internal_tls_options()` in cdbconn.c builds the libpq options
  that dispatch, the FTS probe and gp_inject_fault connect with.
- The tests are in src/test/ssl/t/003_gp_internal_tls.pl. A demo cluster
  created with `WITH_INTERNAL_TLS=1` runs with `verify-ca`
  (gpAux/gpdemo/setup_internal_tls.sh).
