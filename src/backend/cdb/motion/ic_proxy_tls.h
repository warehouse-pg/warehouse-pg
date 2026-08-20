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
 * On success the SSL_CTX getters below return non-NULL contexts. If
 * gp_interconnect_proxy_tls_enable is off, init is a no-op and the
 * contexts stay NULL.
 *
 * Init either:
 *   - Loads cert / key from the gp_interconnect_proxy_tls_*_file GUCs
 *     if all are configured (full PKI mode); or
 *   - Generates an ephemeral self-signed P-256 cert valid for one year
 *     (encrypted-on-wire, not MITM-safe; falls back when the operator
 *     hasn't configured a cluster CA).
 *
 * Errors ereport ERROR and tear down the bgworker — a misconfigured
 * proxy must fail at startup, not at first peer handshake.
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
 * Per-role SSL_CTXs.
 *
 * Both halves of every peer pair handshake — client side (the proxy
 * that initiates connect) and server side (the proxy that accepted).
 * Two contexts because OpenSSL ties ALPN selection callback and a
 * couple of other knobs to the connect/accept role.
 *
 * NULL when ic_proxy_tls_is_enabled() is false.
 */
extern SSL_CTX *ic_proxy_tls_server_ctx(void);
extern SSL_CTX *ic_proxy_tls_client_ctx(void);

/*
 * Per-peer TLS state.
 *
 * Allocated once per ICProxyPeer when ic_proxy_tls_is_enabled().
 * Holds the SSL plus a socket BIO that wraps the peer's underlying
 * file descriptor directly. This lets OpenSSL drive recv/send on
 * the fd itself — when the running kernel supports kTLS AND
 * SSL_OP_ENABLE_KTLS was set on the SSL_CTX, OpenSSL's handshake
 * completion uploads the AEAD keys to the kernel via setsockopt,
 * and subsequent SSL_read/SSL_write become wrappers over kernel
 * sendmsg/recvmsg with the AEAD running in-kernel (or in NIC, if
 * the NIC has TLS offload). When the kernel does NOT support
 * kTLS, OpenSSL stays on userspace AEAD over the same socket BIO
 * — the on-wire TLS 1.3 records and the application-visible
 * SSL_read/SSL_write API are identical, only the cost of crypto
 * differs.
 *
 * libuv can't read/write the same fd via uv_tcp_t while OpenSSL
 * is also reading/writing through the socket BIO (two epoll
 * registrations conflict). The peer code therefore moves the fd
 * out of its uv_tcp_t and onto a uv_poll_t before TLS setup; see
 * ic_proxy_peer.c for the fd-handoff sequencing.
 */
typedef struct ICProxyTlsConn ICProxyTlsConn;

/*
 * Result of ic_proxy_tls_conn_handshake_step.
 */
typedef enum
{
	IC_PROXY_TLS_HS_DONE = 0,			/* handshake finished, switch to data plane */
	IC_PROXY_TLS_HS_WANT_READ,			/* need a UV_READABLE poll event */
	IC_PROXY_TLS_HS_WANT_WRITE,			/* need a UV_WRITABLE poll event */
	IC_PROXY_TLS_HS_FATAL = -1,			/* unrecoverable; close the peer */
} ICProxyTlsHandshakeResult;

/*
 * Allocate a per-peer TLS state attached to fd. is_server selects
 * which SSL_CTX to use and whether SSL is put into accept or
 * connect state.
 *
 * The fd MUST be a connected non-blocking TCP socket; libuv must
 * not be reading/writing it via uv_tcp_t at this point (caller's
 * responsibility — typically take fd out of uv_tcp_t via uv_close
 * + dup before calling here).
 *
 * Returns NULL if TLS is not enabled (caller should treat the peer
 * as plain-TCP).
 */
extern ICProxyTlsConn *ic_proxy_tls_conn_new(int fd, bool is_server);

/*
 * Release the SSL + BIO machinery. The underlying fd is NOT closed
 * (BIO_NOCLOSE on the socket BIO) — the caller is responsible for
 * close()ing the fd after free.
 *
 * Idempotent on NULL.
 */
extern void ic_proxy_tls_conn_free(ICProxyTlsConn *conn);

/*
 * Drive one step of the handshake. Calls SSL_do_handshake against
 * the socket BIO; OpenSSL reads/writes the fd directly as needed.
 *
 * Returns:
 *   IC_PROXY_TLS_HS_DONE       — handshake finished. Caller should
 *                                check BIO_get_ktls_send/recv to log
 *                                whether kTLS engaged, then transition
 *                                to the data-plane code path.
 *   IC_PROXY_TLS_HS_WANT_READ  — needs a UV_READABLE event before
 *                                making more progress.
 *   IC_PROXY_TLS_HS_WANT_WRITE — needs a UV_WRITABLE event before
 *                                making more progress.
 *   IC_PROXY_TLS_HS_FATAL      — unrecoverable; close peer.
 */
extern ICProxyTlsHandshakeResult
ic_proxy_tls_conn_handshake_step(ICProxyTlsConn *conn);

/*
 * Whether the handshake has completed and the data plane is open.
 */
extern bool ic_proxy_tls_conn_handshake_done(const ICProxyTlsConn *conn);

/*
 * Whether kTLS is engaged on the TX path / RX path. Both true means
 * the kernel is doing the AEAD; either false means OpenSSL is
 * handling AEAD in userspace over the socket BIO. Use only after
 * handshake_done; the values are stable for the life of the conn.
 */
extern bool ic_proxy_tls_conn_ktls_tx(const ICProxyTlsConn *conn);
extern bool ic_proxy_tls_conn_ktls_rx(const ICProxyTlsConn *conn);

/*
 * SSL_read / SSL_write semantics. Same return-value convention as
 * ic_proxy_tls_conn_handshake_step's WANT_*: positive number means
 * bytes successfully processed, 0 means WANT_READ / WANT_WRITE
 * (caller arms the corresponding poll event and retries on
 * readiness), -1 means fatal.
 *
 * The out_event argument reports the direction OpenSSL said it needs
 * next on a 0 return: UV_READABLE for SSL_ERROR_WANT_READ, UV_WRITABLE
 * for SSL_ERROR_WANT_WRITE.  It is advisory: the peer data plane
 * re-arms its poll from its own state (read cb armed → UV_READABLE,
 * tx queue non-empty → UV_WRITABLE) rather than from out_event.
 * Cross-direction WANTs cannot happen once kTLS is engaged, and on the
 * userspace-AEAD path they are limited to a TLS 1.3 KeyUpdate hitting
 * a full socket buffer — which resolves on the next poll cycle since
 * reads stay armed for the life of the data plane.
 */
extern int ic_proxy_tls_conn_read(ICProxyTlsConn *conn,
								  void *buf, int len, int *out_event);
extern int ic_proxy_tls_conn_write(ICProxyTlsConn *conn,
								   const void *buf, int len, int *out_event);

/*
 * Bytes of decrypted plaintext buffered inside the SSL object
 * (SSL_pending).  These raise no poll event — uv_poll only sees
 * socket-level bytes — so a reader must drain them explicitly when it
 * (re)arms a read.
 */
extern int ic_proxy_tls_conn_pending(const ICProxyTlsConn *conn);

#endif /* ENABLE_IC_PROXY */

#endif /* IC_PROXY_TLS_H */
