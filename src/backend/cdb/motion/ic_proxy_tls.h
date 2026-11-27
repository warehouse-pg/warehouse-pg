/*-------------------------------------------------------------------------
 *
 * ic_proxy_tls.h
 *	  TLS for ic-proxy peer (daemon-to-daemon) connections.
 *
 * Encrypts inter-host proxy traffic. Backend-to-daemon Unix-domain-socket
 * traffic is unaffected (local trust boundary).
 *
 *
 * Copyright (c) 2026-Present.
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef IC_PROXY_TLS_H
#define IC_PROXY_TLS_H

#ifdef ENABLE_IC_PROXY

struct sockaddr;

/*
 * ALPN protocol identifier of the ic-proxy peer protocol.  The client
 * advertises it, the server selects it, and both ends drop a connection
 * whose handshake did not negotiate it; this turns away TLS endpoints
 * that are not ic-proxy peers (e.g. a misdirected HTTPS scanner),
 * including those that send no ALPN at all.
 */
#define IC_PROXY_TLS_ALPN_STRING	"ic-proxy/1"

/*
 * Process-wide TLS settings.
 *
 * ic_proxy_tls_init is called from ic_proxy_server_main() before any peer
 * can connect.  If gp_interconnect_proxy_tls_enable is off it is a no-op.
 * Otherwise it either:
 *   - Loads the certificate chain + key named by the
 *     gp_interconnect_proxy_tls_cert_file / key_file GUCs, which must be
 *     set together, and enables mutual verification when ca_file is set
 *     as well (ca_file without cert/key is rejected: the ephemeral cert
 *     below could never verify against it); or
 *   - Generates an ephemeral self-signed P-256 cert valid for one year
 *     (encrypted-on-wire, not MITM-safe; the default when no cert is
 *     configured).
 *
 * Errors ereport ERROR — a misconfigured proxy must fail at startup, not
 * at the first peer handshake.  ic_proxy_server_main catches it and parks
 * the bgworker until the configuration is reloaded.
 *
 * ic_proxy_tls_reload is called after a configuration reload.  It reads
 * the certificate, key and CA again, so they can be replaced without a
 * restart: new connections use the new ones, established connections are
 * left alone.  It never raises: when the new settings cannot be used the
 * error is logged and the previous settings stay in effect.
 */
extern void ic_proxy_tls_init(void);
extern void ic_proxy_tls_reload(void);
extern void ic_proxy_tls_uninit(void);

/*
 * Effective enable state.
 *
 * Returns true iff gp_interconnect_proxy_tls_enable is on AND
 * ic_proxy_tls_init() succeeded. Peer code reads this on every
 * connect/accept to decide whether to drive a handshake.
 */
extern bool ic_proxy_tls_is_enabled(void);

/*
 * Per-peer TLS state.
 *
 * Allocated once per ICProxyPeer when ic_proxy_tls_is_enabled().  The SSL
 * object is wired to two memory BIOs, so OpenSSL never touches the socket:
 * the peer code feeds it the ciphertext libuv read from peer->tcp, takes
 * the ciphertext OpenSSL produced and writes it with uv_write.  libuv keeps
 * owning the socket, its read / write / shutdown machinery is untouched,
 * and the TLS layer is a pure byte filter between the two.
 */
typedef struct ICProxyTlsConn ICProxyTlsConn;

/*
 * Result of ic_proxy_tls_conn_handshake_step.
 */
typedef enum
{
	IC_PROXY_TLS_HS_DONE = 0,			/* handshake finished, switch to data plane */
	IC_PROXY_TLS_HS_WANT_READ,			/* needs more ciphertext from the peer */
	IC_PROXY_TLS_HS_FATAL = -1,			/* unrecoverable; shut the peer down */
} ICProxyTlsHandshakeResult;

/*
 * Allocate the per-peer TLS state.  is_server selects which SSL_CTX to use
 * and whether SSL is put into accept or connect state.  name is the peer's
 * name buffer, used in log messages; it must outlive the connection.
 * remote is the address of the other end, by which the reports of failing
 * connections are rate limited.  Returns NULL if TLS is not enabled.
 */
extern ICProxyTlsConn *ic_proxy_tls_conn_new(bool is_server, const char *name,
											 const struct sockaddr *remote);

/*
 * The level to report a connection at that failed before it carried any
 * data: WARNING for the first failure with its remote address in a
 * minute, DEBUG1 for the others, which are counted and summed up with the
 * next WARNING.  A proxy retries a lost peer every second; without this a
 * peer that cannot be talked to floods the log.  For the peer code's own
 * reports (the handshake timeout); the failures detected in here use it
 * already.
 */
extern int ic_proxy_tls_conn_failure_elevel(ICProxyTlsConn *conn);

/*
 * Release the SSL and its BIOs.  Idempotent on NULL.
 */
extern void ic_proxy_tls_conn_free(ICProxyTlsConn *conn);

/*
 * Hand ciphertext received from the socket to OpenSSL.  Always takes all
 * of it (memory BIO).
 */
extern void ic_proxy_tls_conn_feed(ICProxyTlsConn *conn,
								   const void *data, int len);

/*
 * Drive the handshake with whatever ciphertext has been fed so far.  Any
 * ciphertext it produces waits in the output BIO; the caller flushes it
 * with ic_proxy_tls_conn_output_take.  The caller must flush after every
 * step, also after DONE (the client's Finished is produced there).
 */
extern ICProxyTlsHandshakeResult
ic_proxy_tls_conn_handshake_step(ICProxyTlsConn *conn);

/*
 * Whether the handshake has completed and the data plane is open.
 */
extern bool ic_proxy_tls_conn_handshake_done(const ICProxyTlsConn *conn);

/*
 * Plaintext out of the fed ciphertext.  Returns the number of bytes
 * placed in buf, 0 when more ciphertext is needed, -1 on close_notify
 * from the peer, after our own close_notify, or on a fatal error (logged).
 */
extern int ic_proxy_tls_conn_read(ICProxyTlsConn *conn, void *buf, int len);

/*
 * Whether input is waiting that has not been returned yet: ciphertext fed
 * but not yet consumed by OpenSSL, or plaintext it already decrypted.
 * Such bytes raise no read event on the socket, so a reader that (re)arms
 * itself must drain them explicitly.
 */
extern bool ic_proxy_tls_conn_has_pending(const ICProxyTlsConn *conn);

/*
 * Encrypt plaintext into the output BIO.  Returns false after our
 * close_notify or on a fatal error (logged).  Also rotates the traffic
 * keys (TLS 1.3 KeyUpdate) before a connection encrypts more records than
 * is safe under one AES-GCM key; the KeyUpdate record lands in the output
 * BIO ahead of the data.
 */
extern bool ic_proxy_tls_conn_write(ICProxyTlsConn *conn,
									const void *buf, int len);

/*
 * Ciphertext waiting to be written to the socket, and how to take it.
 */
extern int ic_proxy_tls_conn_output_pending(const ICProxyTlsConn *conn);
extern int ic_proxy_tls_conn_output_take(ICProxyTlsConn *conn,
										 void *buf, int len);

/*
 * The socket reached EOF or failed: nothing will be fed any more and
 * nothing can be sent, so a later ic_proxy_tls_conn_shutdown must not
 * queue close_notify (the remote is gone, the write would only draw a
 * reset).
 */
extern void ic_proxy_tls_conn_remote_closed(ICProxyTlsConn *conn);

/*
 * Queue close_notify in the output BIO.  Skipped when the handshake never
 * completed, a fatal error was already reported or the remote is gone;
 * never sent twice.  No read or write may follow it.
 */
extern void ic_proxy_tls_conn_shutdown(ICProxyTlsConn *conn);

#endif /* ENABLE_IC_PROXY */

#endif /* IC_PROXY_TLS_H */
