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

#include <openssl/ssl.h>

/*
 * ALPN protocol identifier advertised by every ic-proxy peer
 * handshake. Both sides require this match — refuses connections
 * from accidental TLS clients that happen to land on the proxy
 * port (e.g. a misdirected HTTPS scanner).
 */
#define IC_PROXY_TLS_ALPN_STRING	"ic-proxy/1"

/*
 * Process-wide TLS state.
 *
 * Called once from ic_proxy_server_main() before any peer can connect.
 * If gp_interconnect_proxy_tls_enable is off, init is a no-op.
 *
 * Init either:
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
 * at the first peer handshake.  ic_proxy_server_main catches it and exits
 * the bgworker for good (all the TLS GUCs need a restart anyway).
 */
extern void ic_proxy_tls_init(void);
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
 * Returns NULL if TLS is not enabled.
 */
extern ICProxyTlsConn *ic_proxy_tls_conn_new(bool is_server, const char *name);

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
 * close_notify or on a fatal error (logged).
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
