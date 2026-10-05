---
title: Securing QD-to-QE Internal Connections with Mutual TLS
---

# Securing QD-to-QE Internal Connections with Mutual TLS

Warehouse-PG dispatches queries and runs Fault Tolerance Service (FTS) probes over
internal libpq connections from the coordinator (QD) to the segments (QE).
Historically these connections were identified by a special bit in the protocol
version (the "internal connection" marker) and were granted access **without any
authentication** — on a segment the bypass was unconditional. Any host able to
reach a segment's PostgreSQL port could therefore obtain superuser access with no
credentials simply by setting that bit.

The `gp_internal_tls` feature replaces this with **mutual TLS (mTLS)**: the QD
presents a client certificate signed by a cluster Certificate Authority (CA), and
the QE verifies it during the TLS handshake. The certificate *is* the
authentication; the protocol marker is demoted to a mere connection-type hint and
no longer grants any trust on its own.

## The two trust domains

Keep the cluster's internal identity separate from the coordinator's external
identity. They are different trust domains and must use **different CAs**:

|Domain|GUCs|Used for|Signed by|
|------|----|--------|---------|
|External server SSL|`ssl_cert_file`, `ssl_key_file`, `ssl_ca_file`|Coordinator serving client applications (psql, drivers)|Your corporate/public CA|
|Internal cluster mTLS|`gp_internal_tls_cert_file`, `gp_internal_tls_key_file`, `gp_internal_tls_ca_file`|QD↔QE dispatch and FTS connections|Your private **cluster CA**|

In PostgreSQL the **server** certificate (`ssl_*`) and the **client** certificate
(libpq connection parameters) are independent:

- A **segment** only ever serves internal connections, so its `ssl_*` server
  certificate is its cluster identity (signed by the cluster CA), and it verifies
  incoming QD client certificates against `gp_internal_tls_ca_file`.
- The **coordinator** serves external clients, so its `ssl_*` is the
  external-facing certificate; it dials segments as a *client* using
  `gp_internal_tls_*` (the cluster identity). Internal connections *into* the
  coordinator (entry-DB / QE-at-coordinator) use a local Unix-domain socket and
  are trusted by file-system permissions — they neither use nor require TLS.

Crucially, the accepting node verifies an internal peer certificate against
`gp_internal_tls_ca_file`, **not** against `ssl_ca_file`. The server loads both
CAs into its TLS verify store (so a cluster certificate is not rejected during
the handshake even on a coordinator whose `ssl_ca_file` is the external CA), but
internal authentication then re-verifies the peer specifically against the
cluster CA. A leaf issued by the external `ssl_ca_file` therefore cannot be used
as a cluster credential, and a coordinator that serves external clients over
one-way TLS need not set `ssl_ca_file` at all.

There is **no fallback** from `gp_internal_tls_*` to `ssl_*`. When
`gp_internal_tls = verify-ca` the postmaster refuses to start unless
`gp_internal_tls_cert_file`, `gp_internal_tls_key_file` and
`gp_internal_tls_ca_file` are all set, so a coordinator can never accidentally
dial segments with its external-facing certificate (which the cluster CA would
reject). On a pure-internal cluster, set them to the same cluster certificate as
`ssl_*`; on a coordinator that serves external clients, set them to the cluster
certificate while `ssl_*` keeps the external one.

## The `gp_internal_tls` GUC

`gp_internal_tls` (`PGC_POSTMASTER`, changing it requires a cluster restart) takes
one of two values, named after the libpq `sslmode` they resolve to on the wire:

- `disable` (default) — legacy plaintext; the protocol marker alone is honored.
  This preserves backwards compatibility for clusters that have not yet deployed
  certificates. It is insecure on its own.
- `verify-ca` — mandatory mutual TLS. The internal short-circuit is honored only
  after the peer certificate verifies against the internal cluster CA
  (`gp_internal_tls_ca_file`); anything else is rejected. This is the secure end
  state. There is no plaintext fallback: once TLS is configured, a connection
  that cannot be mutually authenticated fails rather than silently downgrading.

There is deliberately no intermediate "prefer" mode. `gp_internal_tls` is
`PGC_POSTMASTER` and is applied cluster-wide by a single restart, so there is no
node-by-node rollout for such a mode to smooth, and a mode that silently falls
back to plaintext is exactly the footgun this feature retires.

When `gp_internal_tls = verify-ca`, the postmaster refuses to start unless SSL is
enabled (`ssl = on`) and the internal client credentials
(`gp_internal_tls_cert_file`, `gp_internal_tls_key_file`,
`gp_internal_tls_ca_file`) are all set. Note that `ssl_ca_file` is **not**
required — internal peers are verified against `gp_internal_tls_ca_file`. A
misconfiguration fails fast at startup instead of silently locking the segment
out of the cluster.

Before switching to `verify-ca`, validate the certificate wiring out of band —
for example, on each node run `openssl verify -CAfile <cluster-CA> <node-cert>`
and confirm a cross-node TLS connection succeeds — because a certificate that is
present but does not verify only surfaces at connection time, not at startup.

## Enabling mutual TLS

1. Create a private **cluster CA** and issue a node certificate for each host,
   with the host's name / addresses in the Subject Alternative Name.
2. Distribute the node certificate, key and the cluster CA certificate to every
   data directory (certificate-file distribution is a DBA responsibility; the
   GUCs only point at the files). Relative paths are resolved against the data
   directory, as for the built-in `ssl_*` GUCs.
3. On each node set, via `gpconfig`:
   - everywhere: `ssl = on`, and `gp_internal_tls_cert_file`,
     `gp_internal_tls_key_file`, `gp_internal_tls_ca_file` to the cluster
     certificate/key/CA (the internal client identity; required, no fallback).
   - segments: `ssl_cert_file`, `ssl_key_file` to the cluster certificate/key
     (their server identity is the cluster identity). `ssl_ca_file` is not needed
     for internal authentication.
   - coordinator: `ssl_cert_file`, `ssl_key_file` (and `ssl_ca_file` only if it
     verifies external client certificates) to the external-facing certificate it
     presents to client applications.
   - everywhere: `gp_internal_tls = verify-ca`.
4. Restart the cluster: `gpstop -ar`.

Because `gp_internal_tls` is `PGC_POSTMASTER`, a normal maintenance-window restart
(`gpstop -ar`) brings the whole cluster up in `verify-ca` at once, with no mixed
state, so you go straight from `disable` to `verify-ca`. Validate the certificate
wiring first (see above); a node whose certificate does not verify is only
rejected when a connection is attempted, not at startup.

## Troubleshooting

A failed handshake now logs the offending certificate so a CA mismatch is obvious.
For example, a QD dialing a segment with the wrong (external) certificate produces
on the segment:

```
LOG:  SSL certificate verification failed at depth 0: unable to get local issuer certificate
DETAIL:  Failing certificate: subject "/CN=seg1.example.com", issuer "/CN=EXTERNAL-corp-CA".
```

The `issuer` line shows the cluster expected its internal CA but received a
certificate from a different one. On the QD side, raise `log_min_messages` to
`debug2` to see the `sslmode` and certificate files chosen for each internal
connection. A connection rejected purely for lack of a certificate reports:

```
FATAL:  internal connection rejected: a verified client certificate is required
DETAIL:  gp_internal_tls is set to "verify-ca"; QD-to-QE connections must authenticate with mutual TLS against the internal cluster CA (gp_internal_tls_ca_file).
```

## Notes

- No catalog, ABI, or WAL changes — this is a drop-in feature.
- Connections over a Unix-domain socket (entry-DB / QE-at-coordinator) are always
  trusted locally and are never required to use TLS.
- Parallel retrieve (`gp_role=retrieve`) connections are not internal-marker
  connections and are unaffected by `gp_internal_tls`.
