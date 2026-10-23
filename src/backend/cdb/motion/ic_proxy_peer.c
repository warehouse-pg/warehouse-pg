/*-------------------------------------------------------------------------
 *
 * ic_proxy_server_peer.c
 *
 *    Interconnect Proxy Peer
 *
 * A peer lives in the proxy bgworker and connects to a proxy on an other
 * segment.  When there are N segments, including the coordinator, a proxy bgworker
 * needs to connect to all the other (N - 1) segments, the same amount of peers
 * are needed, too.
 *
 * A peer is identified with the dbid, so two different peers are used to
 * connect to a remote segment's primary and mirror.  The proxy bgworker is not
 * launched on a mirror until it is promoted, so most of time there is only the
 * peer to the segment's primary, but there is a chance for the peer to the
 * mirror to live together with the primary one, this happends during the
 * mirror promotion.
 *
 * There are only one proxy connection between two proxies, a rule is put here
 * that the proxy on segment X connects to the one on segment Y iff X > Y, not
 * the reverse.  This rule is true even if X or Y crashes and relaunches the
 * proxy bgworker.
 *
 * Peers always communicate to each other via ICProxyPkt, a connection must
 * begin with the hand shaking messages.  A hand shaking is needed for a pair
 * of peers to know the information of each other, such as the dbids.
 *
 * Clients can send packets before the peer hand shaking is finished, in such a
 * case a placeholder is registered to hold the early outgoing packets.  Once
 * the peer finishes the hand shaking it replaces the placeholder and handles
 * these early packets in the arriving order.
 *
 * Incoming packets, the one received from a remote peer, is never cached in
 * the peer, they are routed to the target clients, or their placeholders,
 * immediately.
 *
 *
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates.
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <netinet/tcp.h>		/* TCP_CORK */
#include <sys/socket.h>			/* setsockopt() */
#include <unistd.h>				/* close(), dup() */

#include "ic_proxy_server.h"
#include "ic_proxy_pkt_cache.h"
#include "ic_proxy_addr.h"
#include "ic_proxy_tls.h"

#include <uv.h>


/*
 * The peer register table, the peer with dbid is stored in [dbid].
 *
 * TODO: not using a fixed length array.
 */
static ICProxyPeer *ic_proxy_peers[65536];


static void ic_proxy_peer_shutdown(ICProxyPeer *peer);
static void ic_proxy_peer_handle_out_cache(ICProxyPeer *peer);
static void ic_proxy_peer_on_data_pkt(void *opaque,
									  const void *data, uint16 size);
static void ic_proxy_peer_send_message(ICProxyPeer *peer,
									   ICProxyMessageType mtype,
									   const ICProxyKey *key,
									   ic_proxy_sent_cb callback);

/*
 * --- TLS plumbing overview ---
 *
 * When `gp_interconnect_proxy_tls_enable=on`, the per-peer TCP
 * connection migrates out of libuv's uv_tcp_t handle and onto a
 * uv_poll_t backed by a dup'd fd that OpenSSL drives through a socket
 * BIO. The migration happens immediately after connect / accept (i.e.
 * before any HELLO message), so the entire peer-protocol traffic is
 * encrypted. ic_proxy_tls.h has the full architecture; ic_proxy_tls.c
 * owns the SSL_CTXs.
 *
 * fd handoff (irreversible, both directions of a peer connection):
 *   1. peer->tcp is connected (client side) or accepted (server side)
 *   2. ic_proxy_peer_tls_start:
 *        a. uv_fileno + dup → peer->tls_fd
 *        b. uv_close(&peer->tcp, on_tcp_closed_for_{client,server})
 *   3. on_tcp_closed_for_{client,server} → ic_proxy_peer_tls_after_close:
 *        a. uv_poll_init_socket(&peer->tls_poll, peer->tls_fd)
 *        b. ic_proxy_tls_conn_new(fd, is_server)
 *        c. arm poll for the handshake's initial direction
 *   4. ic_proxy_peer_tls_on_poll → step_handshake → post_handshake →
 *      data plane (drain_rx + drain_tx)
 *
 * Why the dup: libuv internally registers the fd with epoll when you
 * uv_read_start a uv_tcp_t. Re-registering the same fd onto a
 * uv_poll_t collides — EEXIST from epoll_ctl. We dup so the kernel
 * sees two distinct epoll registrations even though they alias the
 * same socket file. The original fd is closed when libuv finalises
 * uv_close(peer->tcp); the dup'd fd is closed when we uv_close
 * peer->tls_poll and the close-cb runs.
 *
 * Why uv_poll_t at all: OpenSSL's socket BIO calls recv/send directly,
 * which is incompatible with libuv's uv_read_start (libuv expects to
 * own all reads on the fd). With uv_poll_t we get readiness events
 * (UV_READABLE / UV_WRITABLE) and drive SSL_read / SSL_write
 * ourselves. This is also the only mode where OpenSSL can engage
 * kernel kTLS — once handshake completes the AEAD runs in the kernel
 * (or NIC if it supports TLS offload), but SSL_read / SSL_write keep
 * the same API.
 *
 * Write coalescing (commit 7751144ec39 + follow-ups):
 *   - tls_tx_queue holds packets queued by ic_proxy_router_write
 *     while the kernel was full or the handshake was in progress.
 *   - drain_tx packs as many queued packets as fit into a 64 KB
 *     per-peer scratch buffer, then issues ONE SSL_write — that
 *     emits one TLS record with the AEAD overhead amortised across
 *     many packets. SSL_MODE_ENABLE_PARTIAL_WRITE +
 *     SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER are set so OpenSSL accepts
 *     this pattern.
 *   - TCP_CORK is set around the drain so several small SSL_writes
 *     (during a partial-progress cycle) coalesce into one sendmsg.
 *   - Per-peer freelist for ICProxyTlsTxItem and a process-wide one
 *     for ICProxyWriteReq avoid palloc/pfree per packet on the hot
 *     path.
 *
 * Poll-arm dedup:
 *   - tls_poll_armed_events caches the last event mask passed to
 *     uv_poll_start. The data plane often re-arms with the same
 *     mask after every read/write event; skipping the syscall when
 *     the mask is unchanged removes the EPOLL_CTL_MOD round trip.
 *   - 0 means stopped; we don't elide stop→start transitions.
 *
 * Failure paths:
 *   - Handshake fatal → ic_proxy_peer_close (drops the peer + closes
 *     tls_poll + close(tls_fd) in the close-cb).
 *   - mid-data fatal → same path. The peer code never reuses a
 *     half-closed TLS session.
 */

/* TLS plumbing — forward decls; bodies further down. */
static void ic_proxy_peer_client_send_hello(ICProxyPeer *peer);
static void ic_proxy_peer_tls_start(ICProxyPeer *peer, bool is_server);
static void ic_proxy_peer_tls_on_tcp_closed_for_client(uv_handle_t *handle);
static void ic_proxy_peer_tls_on_tcp_closed_for_server(uv_handle_t *handle);
static void ic_proxy_peer_tls_after_close(ICProxyPeer *peer, bool is_server);
static void ic_proxy_peer_tls_on_poll(uv_poll_t *handle, int status, int events);
static void ic_proxy_peer_tls_step_handshake(ICProxyPeer *peer);
static void ic_proxy_peer_tls_post_handshake(ICProxyPeer *peer);
static void ic_proxy_peer_tls_drain_rx(ICProxyPeer *peer);
static void ic_proxy_peer_tls_drain_tx(ICProxyPeer *peer);
static void ic_proxy_peer_tls_rearm_poll(ICProxyPeer *peer);
static void ic_proxy_peer_tls_poll_arm(ICProxyPeer *peer, int events);
typedef struct ICProxyTlsTxItem ICProxyTlsTxItem;
static ICProxyTlsTxItem *ic_proxy_peer_tls_tx_item_get(ICProxyPeer *peer);
static void ic_proxy_peer_tls_tx_item_put(ICProxyPeer *peer,
										  ICProxyTlsTxItem *item);
static void ic_proxy_peer_tls_fail_tx_queue(ICProxyPeer *peer, int status);

/*
 * Wrapper around uv_read_start for peer connections. When the peer
 * has migrated to TLS / uv_poll_t mode, the caller's cb is stashed
 * in peer->tls_real_read_cb and we just ensure poll is armed for
 * UV_READABLE — the SSL_read drain loop invokes the cb with
 * plaintext bytes in pkt-cache buffers, matching the uv_read_cb
 * signature. On plain TCP we call uv_read_start as before.
 */
static inline int
ic_proxy_peer_uv_read_start(ICProxyPeer *peer, uv_read_cb cb)
{
	if (peer->tls != NULL)
	{
		peer->tls_real_read_cb = cb;

		/*
		 * Plaintext OpenSSL already buffered won't raise a poll event —
		 * uv_poll only signals socket-level bytes.  Deliver it now, or a
		 * record that carried more than the previous reader consumed
		 * would sit in the SSL object until the peer happens to send
		 * more data.
		 */
		if (ic_proxy_tls_conn_pending(peer->tls) > 0)
			ic_proxy_peer_tls_drain_rx(peer);

		ic_proxy_peer_tls_rearm_poll(peer);
		return 0;
	}
	peer->tls_real_read_cb = NULL;
	return uv_read_start((uv_stream_t *) &peer->tcp,
						 ic_proxy_pkt_cache_alloc_buffer,
						 cb);
}

/*
 * Wrapper around uv_read_stop for peer connections.  On plain TCP it
 * stops reads on peer->tcp as before.  In TLS / uv_poll_t mode the
 * tcp handle is already closed and uv_read_stop on it would be a
 * silent no-op — the actual "stop" is clearing tls_real_read_cb, which
 * both stops the SSL_read drain loop mid-cycle (it re-reads the cb on
 * every iteration) and makes rearm_poll drop UV_READABLE.
 */
static inline void
ic_proxy_peer_uv_read_stop(ICProxyPeer *peer)
{
	if (peer->tls != NULL)
	{
		peer->tls_real_read_cb = NULL;
		ic_proxy_peer_tls_rearm_poll(peer);
		return;
	}
	uv_read_stop((uv_stream_t *) &peer->tcp);
}


/*
 * Build a delayed packet.
 *
 * We'll take the packet's ownership.
 */
ICProxyDelay *
ic_proxy_peer_build_delay(ICProxyPeer *peer, ICProxyPkt *pkt,
						  ic_proxy_sent_cb callback, void *opaque)
{
	ICProxyDelay *delay;

	delay = ic_proxy_new(ICProxyDelay);
	delay->content = peer ? peer->content : IC_PROXY_INVALID_CONTENT;
	delay->dbid = peer ? peer->dbid : IC_PROXY_INVALID_DBID;
	delay->pkt = pkt;
	delay->callback = callback;
	delay->opaque = opaque;

	return delay;
}

/*
 * Initialize the peer register table.
 */
void
ic_proxy_peer_table_init(void)
{
	memset(ic_proxy_peers, 0, sizeof(ic_proxy_peers));
}

void
ic_proxy_peer_table_uninit(void)
{
	/*
	 * nothing to do for the peers table:
	 * - no need to clear the peers table, we will do that in init();
	 * - no need to free the peers, they should already freed themselves;
	 */
}

/*
 * Update the peer name from the state bits.
 *
 * This function is usually called during logging, so it is good practice not
 * to generate messages in this function.
 */
static void
ic_proxy_peer_update_name(ICProxyPeer *peer)
{
	struct sockaddr_storage peeraddr;
	int			addrlen = sizeof(peeraddr);
	char		sockname[HOST_NAME_MAX] = "";
	char		peername[HOST_NAME_MAX] = "";
	int			sockport = 0;
	int			peerport = 0;

	/*
	 * Show the tcp level connection information in the name, they are not very
	 * useful, though.
	 *
	 * Return codes from ic_proxy_extract_addr() are ignored, as logging should
	 * be avoided in this place.  On the other hand the failures are reflected
	 * in the hostnames and ports, as well as the peer name, so we know it
	 * happens.
	 */
	uv_tcp_getsockname(&peer->tcp, (struct sockaddr *) &peeraddr, &addrlen);
	ic_proxy_extract_sockaddr((struct sockaddr *) &peeraddr,
							  sockname, sizeof(sockname),
							  &sockport, NULL /* family */);

	uv_tcp_getpeername(&peer->tcp, (struct sockaddr *) &peeraddr, &addrlen);
	ic_proxy_extract_sockaddr((struct sockaddr *) &peeraddr,
							  peername, sizeof(peername),
							  &peerport, NULL /* family */);

	snprintf(peer->name, sizeof(peer->name), "peer%s[seg%hd,dbid%hu %s:%d->%s:%d]",
			 (peer->state & IC_PROXY_PEER_STATE_LEGACY) ? ".legacy" : "",
			 peer->content, peer->dbid, sockname, sockport, peername, peerport);
}

/*
 * Unregister a peer.
 */
static void
ic_proxy_peer_unregister(ICProxyPeer *peer)
{
	/* invalid peer */
	if (peer->dbid == IC_PROXY_INVALID_DBID ||
		peer->content == IC_PROXY_INVALID_CONTENT)
		return;

	if (ic_proxy_peers[peer->dbid] == peer)
	{
		/* keep the peer as a placeholder */

		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
			   "ic-proxy: %s: unregistered", peer->name);

		/* reset the state */
		peer->state = 0;
		ic_proxy_peer_update_name(peer);
	}
	else if (ic_proxy_peers[peer->dbid])
	{
		/*
		 * if there is already a placeholder, transfer my cached packets to it
		 */
		ICProxyPeer *placeholder = ic_proxy_peers[peer->dbid];

		placeholder->reqs = list_concat(placeholder->reqs, peer->reqs);
		peer->reqs = NIL;

		/* then free the peer */
		ic_proxy_peer_free(peer);
	}
}

/*
 * Register a peer.
 */
static void
ic_proxy_peer_register(ICProxyPeer *peer)
{
	ICProxyPeer *placeholder = ic_proxy_peers[peer->dbid];

	Assert(peer->dbid > 0);

	if (placeholder)
	{
		/*
		 * FIXME: is it possible for a new peer to come before the legacy one
		 * is ready for message?
		 */

		if (placeholder->state & IC_PROXY_PEER_STATE_READY_FOR_MESSAGE)
		{
			/*
			 * This is not actually a placeholder, but a legacy peer, this
			 * happens due to network problem, etc..
			 */
			elog(WARNING, "ic-proxy: %s(state=0x%08x): found a legacy peer %s(state=0x%08x)",
						 peer->name, peer->state,
						 placeholder->name, placeholder->state);

			placeholder->state |= IC_PROXY_PEER_STATE_LEGACY;
			ic_proxy_peer_update_name(placeholder);

			ic_proxy_peer_shutdown(placeholder);
		}
		else
		{
			/* This is an actual placeholder */
			elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
				   "ic-proxy: %s(state=0x%08x): found my placeholder %s(state=0x%08x)",
						 peer->name, peer->state,
						 placeholder->name, placeholder->state);

			if (placeholder->ibuf.len > 0)
				elog(WARNING, "ic-proxy: %s(state=0x%08x): my placeholder %s(state=0x%08x) has %d bytes in ibuf",
							 peer->name, peer->state,
							 placeholder->name, placeholder->state,
							 placeholder->ibuf.len);

			/* TODO: verify that it's really a placeholder */

			/* transfer the cached pkts */
			peer->reqs = list_concat(peer->reqs, placeholder->reqs);
			placeholder->reqs = NIL;

			/* finally free the placeholder */
			ic_proxy_peer_free(placeholder);
		}
	}

	ic_proxy_peers[peer->dbid] = peer;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
		   "ic-proxy: %s: registered", peer->name);
}

/*
 * Lookup a peer with peerid.
 *
 * We require to pass both content and dbid as arguments, but only dbid is
 * used.
 */
ICProxyPeer *
ic_proxy_peer_lookup(int16 content, uint16 dbid)
{
	Assert(dbid > 0);

	return ic_proxy_peers[dbid];
}

/*
 * Lookup a peer with peerid, create a placeholder if not found.
 */
ICProxyPeer *
ic_proxy_peer_blessed_lookup(uv_loop_t *loop, int16 content, uint16 dbid)
{
	Assert(dbid > 0);

	if (!ic_proxy_peers[dbid])
	{
		ICProxyPeer *peer = ic_proxy_peer_new(loop, content, dbid);

		/* register as a placeholder */
		ic_proxy_peer_register(peer);
	}

	return ic_proxy_peers[dbid];
}

/*
 * Received a complete DATA or MESSAGE packet from a remote peer.
 */
static void
ic_proxy_peer_on_data_pkt(void *opaque, const void *data, uint16 size)
{
	const ICProxyPkt *pkt = data;
	ICProxyPeer *peer = opaque;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG5,
		   "ic-proxy: %s: received %s", peer->name, ic_proxy_pkt_to_str(pkt));

	/* sanity check: drop the packet with incorrect magic number */
	if (!ic_proxy_pkt_is_valid(pkt))
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG1,
			"ic-proxy: %s: received %s, dropping the invalid package (magic number mismatch)",
					peer->name, ic_proxy_pkt_to_str(pkt));
		return;
	}

	if (!(peer->state & IC_PROXY_PEER_STATE_READY_FOR_DATA))
	{
		elog(WARNING, "ic-proxy: %s: not ready to receive DATA yet: %s",
					 peer->name, ic_proxy_pkt_to_str(pkt));
		return;
	}

	ic_proxy_router_route(peer->tcp.loop, ic_proxy_pkt_dup(pkt), NULL, NULL);
}

/*
 * Received bytes from a remote peer.
 */
static void
ic_proxy_peer_on_data(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) stream, ICProxyPeer, tcp);

	if (unlikely(nread < 0))
	{
		if (nread != UV_EOF)
			elog(WARNING, "ic-proxy: %s: failed to receive DATA: %s",
						 peer->name, uv_strerror(nread));
		else
			elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
				   "ic-proxy: %s: received EOF while waiting for DATA",
						 peer->name);

		if (buf->base)
			ic_proxy_pkt_cache_free(buf->base);

		ic_proxy_peer_shutdown(peer);
		return;
	}
	else if (unlikely(nread == 0))
	{
		if (buf->base)
			ic_proxy_pkt_cache_free(buf->base);

		/* EAGAIN or EWOULDBLOCK, retry */
		return;
	}

	ic_proxy_ibuf_push(&peer->ibuf, buf->base, nread,
					   ic_proxy_peer_on_data_pkt, peer);
	ic_proxy_pkt_cache_free(buf->base);
}

/*
 * Create a peer.
 */
ICProxyPeer *
ic_proxy_peer_new(uv_loop_t *loop, int16 content, uint16 dbid)
{
	ICProxyPeer *peer;

	peer = ic_proxy_new(ICProxyPeer);
	peer->content = content;
	peer->dbid = dbid;
	peer->state = 0;
	peer->reqs = NIL;
	peer->tls = NULL;
	peer->tls_real_read_cb = NULL;
	peer->tls_fd = -1;
	peer->tls_tx_queue = NIL;
	peer->tls_poll_armed_events = 0;
	peer->tls_tx_scratch = NULL;
	peer->tls_tx_item_freelist = NIL;

	ic_proxy_ibuf_init_p2p(&peer->ibuf);

	uv_tcp_init(loop, &peer->tcp);
	uv_tcp_nodelay(&peer->tcp, true);

	ic_proxy_peer_update_name(peer);

	return peer;
}

/*
 * Free a peer.
 *
 * A peer should only be used if it is really unused.  Most of the time a
 * closed peer is converted to a placeholder, so it should not be freed.  Only
 * a replaced placeholder should be freed.
 */
void
ic_proxy_peer_free(ICProxyPeer *peer)
{
	ListCell   *cell;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG5,
		   "ic-proxy: %s: freeing", peer->name);

	foreach(cell, peer->reqs)
	{
		ICProxyPkt *pkt = lfirst(cell);

		elog(WARNING, "ic-proxy: %s: unhandled outgoing %s, dropping it",
					 peer->name, ic_proxy_pkt_to_str(pkt));

		ic_proxy_pkt_cache_free(pkt);
	}

	list_free(peer->reqs);

	ic_proxy_ibuf_uninit(&peer->ibuf);

	/*
	 * Normally drained by ic_proxy_peer_tls_on_poll_closed; placeholders
	 * never queue writes, so this is a safety net.
	 */
	ic_proxy_peer_tls_fail_tx_queue(peer, UV_ECANCELED);

	ic_proxy_tls_conn_free(peer->tls);
	peer->tls = NULL;
	if (peer->tls_tx_scratch != NULL)
	{
		ic_proxy_free(peer->tls_tx_scratch);
		peer->tls_tx_scratch = NULL;
	}
	if (peer->tls_tx_item_freelist != NIL)
	{
		ListCell   *cell;

		foreach(cell, peer->tls_tx_item_freelist)
			ic_proxy_free(lfirst(cell));
		list_free(peer->tls_tx_item_freelist);
		peer->tls_tx_item_freelist = NIL;
	}
	ic_proxy_free(peer);

	/*
	 * TODO: if a peer disconnected, should we also disconnect all the relative
	 * clients?  The concern is that some packets might already be lost.
	 *
	 * Anyway, future packets should not be cached inside the peer.
	 */
}

/*
 * The peer is closed.
 */
static void
ic_proxy_peer_on_close(uv_handle_t *handle)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) handle, ICProxyPeer, tcp);

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
		   "ic-proxy: %s: closed", peer->name);

	/* reset the state */
	peer->state = 0;

	/* it's unlikely that the ibuf is non-empty, but clear it for sure */
	ic_proxy_ibuf_clear(&peer->ibuf);

	ic_proxy_peer_unregister(peer);
}

/*
 * uv_close callback for the TLS poll handle. Finalises peer state
 * after the handshake-driven poll handle is torn down.
 */
static void
ic_proxy_peer_tls_on_poll_closed(uv_handle_t *handle)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) handle, ICProxyPeer, tls_poll);

	/*
	 * Fail unsent packets while peer->tls is still set, so a callback
	 * reacting with ic_proxy_peer_shutdown takes the TLS branch (whose
	 * close call no-ops on the CLOSING guard) instead of uv_shutdown'ing
	 * the long-closed tcp handle.
	 */
	ic_proxy_peer_tls_fail_tx_queue(peer, UV_ECANCELED);

	if (peer->tls_fd >= 0)
	{
		close(peer->tls_fd);
		peer->tls_fd = -1;
	}
	ic_proxy_tls_conn_free(peer->tls);
	peer->tls = NULL;
	peer->tls_poll_armed_events = 0;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
		   "ic-proxy: %s: closed (tls)", peer->name);

	peer->state = 0;
	ic_proxy_ibuf_clear(&peer->ibuf);
	ic_proxy_peer_unregister(peer);
}

/*
 * Close a peer.
 *
 * A peer could only be closed after its shutdown. TLS-mode peers
 * own a uv_poll_t handle (over a dup'd fd) instead of uv_tcp_t for
 * their active state — see ic_proxy_peer_tls_start.
 */
static void
ic_proxy_peer_close(ICProxyPeer *peer)
{
	if (peer->state & IC_PROXY_PEER_STATE_CLOSING)
		return;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
		   "ic-proxy: %s: closing", peer->name);

	peer->state |= IC_PROXY_PEER_STATE_CLOSING;

	if (peer->tls != NULL)
	{
		/*
		 * TLS poll-mode: peer->tcp was already uv_close'd during
		 * ic_proxy_peer_tls_start, so we only need to wind the
		 * tls_poll handle down. The peer struct memory is freed
		 * later via ic_proxy_peer_free; here we just shut the
		 * libuv handle.
		 */
		uv_poll_stop(&peer->tls_poll);
		uv_close((uv_handle_t *) &peer->tls_poll,
				 ic_proxy_peer_tls_on_poll_closed);
		return;
	}

	uv_close((uv_handle_t *) &peer->tcp, ic_proxy_peer_on_close);
}

/*
 * The peer is shutted down.
 */
static void
ic_proxy_peer_on_shutdown(uv_shutdown_t *req, int status)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) req->handle, ICProxyPeer, tcp);

	ic_proxy_free(req);

	if (status < 0)
		elog(WARNING, "ic-proxy: %s: failed to shutdown: %s",
					 peer->name, uv_strerror(status));

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
		   "ic-proxy: %s: shutted down", peer->name);

	peer->state |= IC_PROXY_PEER_STATE_SHUTTED;

	ic_proxy_peer_close(peer);
}

/*
 * Shutdown a peer.
 */
static void
ic_proxy_peer_shutdown(ICProxyPeer *peer)
{
	uv_shutdown_t *req;

	if (peer->state & IC_PROXY_PEER_STATE_SHUTTING)
		return;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
		   "ic-proxy: %s: shutting down", peer->name);

	peer->state |= IC_PROXY_PEER_STATE_SHUTTING;

	/* disconnect all the clients */
	ic_proxy_client_table_shutdown_by_dbid(peer->dbid);

	if (peer->tls != NULL)
	{
		/*
		 * TLS poll-mode: peer->tcp was already uv_close'd during
		 * ic_proxy_peer_tls_start, so uv_shutdown on it would fail
		 * synchronously without ever invoking ic_proxy_peer_on_shutdown
		 * — the peer would be stuck in SHUTTING forever.  There is no
		 * uv_shutdown equivalent for a uv_poll_t; unsent tx items are
		 * failed during close, so go straight to the SHUTTED state.
		 */
		peer->state |= IC_PROXY_PEER_STATE_SHUTTED;
		ic_proxy_peer_close(peer);
		return;
	}

	req = ic_proxy_new(uv_shutdown_t);

	uv_shutdown(req, (uv_stream_t *) &peer->tcp, ic_proxy_peer_on_shutdown);
}

/*
 * Sent the HELLO ACK message.
 */
static void
ic_proxy_peer_on_sent_hello_ack(void *opaque, const ICProxyPkt *pkt, int status)
{
	ICProxyPeer *peer = opaque;

	if (status < 0)
	{
		ic_proxy_peer_shutdown(peer);
		return;
	}

	peer->state |= IC_PROXY_PEER_STATE_SENT_HELLO_ACK;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy: %s: start receiving DATA", peer->name);

	/* it's unlikely that the ibuf is non-empty, but clear it for sure */
	ic_proxy_ibuf_clear(&peer->ibuf);

	/*
	 * If there are early coming packets, make sure to route them before
	 * receiving new data, we must ensure that packets are routed in the same
	 * order as they arrive.
	 */
	ic_proxy_peer_handle_out_cache(peer);

	/* now it's time to receive the normal data */
	ic_proxy_peer_uv_read_start(peer, ic_proxy_peer_on_data);
}

/*
 * Received the complete HELLO message.
 */
static void
ic_proxy_peer_on_hello_pkt(void *opaque, const void *data, uint16 size)
{
	const ICProxyPkt *pkt = data;
	ICProxyPeer *peer = opaque;
	ICProxyKey	key;

	/* we only expect one hello message */
	ic_proxy_peer_uv_read_stop(peer);

	/* sanity check: drop the packet with incorrect magic number */
	if (!ic_proxy_pkt_is_valid(pkt))
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG1,
			"ic-proxy: %s: received %s, dropping the invalid package (magic number mismatch)",
					peer->name, ic_proxy_pkt_to_str(pkt));
		return;
	}

	ic_proxy_key_from_p2c_pkt(&key, pkt);

	/* TODO: verify that old dbid and content are both set or invalid */
	peer->content = key.remoteContentId;
	peer->dbid = key.remoteDbid;

	ic_proxy_peer_update_name(peer);

	/*
	 * A peer could be registered as long as it knows the peer information from
	 * the HELLO message, the client packets will still be cached until the
	 * HELLO ACK is sent out.
	 */
	ic_proxy_peer_register(peer);

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG1,
		   "ic-proxy: %s: received %s, sending HELLO ACK",
				 peer->name, ic_proxy_pkt_to_str(pkt));

	/*
	 * below two state bits can be merged into one, but it is harmless to keep
	 * them as two.
	 */
	peer->state |= IC_PROXY_PEER_STATE_RECEIVED_HELLO;

	peer->state |= IC_PROXY_PEER_STATE_SENDING_HELLO_ACK;

	ic_proxy_key_reverse(&key);
	key.localPid = MyProcPid;

	ic_proxy_peer_send_message(peer, IC_PROXY_MESSAGE_PEER_HELLO_ACK, &key,
							   ic_proxy_peer_on_sent_hello_ack);
}

/*
 * Received some HELLO bytes.
 */
static void
ic_proxy_peer_on_hello_data(uv_stream_t *stream,
							ssize_t nread, const uv_buf_t *buf)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) stream, ICProxyPeer, tcp);

	if (unlikely(nread < 0))
	{
		if (nread != UV_EOF)
			elog(WARNING, "ic-proxy: %s: failed to receive HELLO: %s",
						 peer->name, uv_strerror(nread));
		else
			elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
				   "ic-proxy: %s: received EOF while waiting for HELLO",
						 peer->name);

		if (buf->base)
			ic_proxy_pkt_cache_free(buf->base);

		ic_proxy_peer_shutdown(peer);
		return;
	}
	else if (unlikely(nread == 0))
	{
		if (buf->base)
			ic_proxy_pkt_cache_free(buf->base);

		/* EAGAIN or EWOULDBLOCK, retry */
		return;
	}

	ic_proxy_ibuf_push(&peer->ibuf, buf->base, nread,
					   ic_proxy_peer_on_hello_pkt, peer);
	ic_proxy_pkt_cache_free(buf->base);
}

/*
 * Start reading the HELLO message.
 */
void
ic_proxy_peer_read_hello(ICProxyPeer *peer)
{
	if (peer->state & IC_PROXY_PEER_STATE_RECEIVING_HELLO)
		return;

	/*
	 * Server side: if TLS is enabled, we must finish the handshake
	 * before any HELLO bytes are exchanged. Hand the fd off to the
	 * uv_poll_t-driven path; ic_proxy_peer_tls_post_handshake will
	 * call back into this function with peer->tls already non-NULL,
	 * falling through to the normal path below (which arms a read
	 * via ic_proxy_peer_uv_read_start — that, with peer->tls set,
	 * routes through SSL_read on the poll loop instead of
	 * uv_read_start on peer->tcp).
	 */
	if (ic_proxy_tls_is_enabled() && peer->tls == NULL)
	{
		ic_proxy_peer_tls_start(peer, true /* is_server */);
		return;
	}

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy: %s: waiting for HELLO", peer->name);

	peer->state |= IC_PROXY_PEER_STATE_RECEIVING_HELLO;

	ic_proxy_peer_uv_read_start(peer, ic_proxy_peer_on_hello_data);
}

/*
 * Received the complete HELLO ACK message.
 */
static void
ic_proxy_peer_on_hello_ack_pkt(void *opaque, const void *data, uint16 size)
{
	const ICProxyPkt *pkt = data;
	ICProxyPeer *peer = opaque;

	if (size < sizeof(*pkt) || size != pkt->len)
		elog(ERROR, "ic-proxy: %s: received incomplete HELLO ACK: size = %d",
					 peer->name, size);

	if (peer->state & IC_PROXY_PEER_STATE_RECEIVED_HELLO_ACK)
	{
		/*
		 * A DATA packet is sent together with the HELLO, so the ibuf push the
		 * DATA here.  I still don't know how would this happen, but this does
		 * happen on the pipeline, so at least let it work.
		 *
		 * TODO: as we can't draw a clear line between handshake and data, it
		 * would be better to merge on_hello* and on_data into one.
		 */
		elog(WARNING, "ic-proxy: %s: early DATA: %s",
					 peer->name, ic_proxy_pkt_to_str(pkt));

		ic_proxy_peer_on_data_pkt(opaque, data, size);
		return;
	}

	/* we only expect one hello ack message */
	ic_proxy_peer_uv_read_stop(peer);

	/* sanity check: drop the packet with incorrect magic number */
	if (!ic_proxy_pkt_is_valid(pkt))
	{
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG1,
			"ic-proxy: %s: received %s, dropping the invalid package (magic number mismatch)",
					peer->name, ic_proxy_pkt_to_str(pkt));
		return;
	}

	if (!ic_proxy_pkt_is(pkt, IC_PROXY_MESSAGE_PEER_HELLO_ACK))
		elog(ERROR, "ic-proxy: %s: received invalid HELLO ACK: %s",
					 peer->name, ic_proxy_pkt_to_str(pkt));

	if (pkt->dstDbid != peer->dbid)
		elog(ERROR, "ic-proxy: %s: received invalid HELLO ACK: %s",
					 peer->name, ic_proxy_pkt_to_str(pkt));

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG1,
		   "ic-proxy: %s: received %s", peer->name, ic_proxy_pkt_to_str(pkt));

	peer->state |= IC_PROXY_PEER_STATE_RECEIVED_HELLO_ACK;

	/* do not clear the ibuf, it could already contain incoming DATA */

	/*
	 * If there are early coming packets, make sure to route them before
	 * receiving new data, we must ensure that packets are routed in the same
	 * order as they arrive.
	 */
	ic_proxy_peer_handle_out_cache(peer);

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy: %s: start receiving DATA", peer->name);

	/* now it's time to receive the normal data */
	ic_proxy_peer_uv_read_start(peer, ic_proxy_peer_on_data);
}

/*
 * Received HELLO ACK bytes.
 */
static void
ic_proxy_peer_on_hello_ack_data(uv_stream_t *stream,
								ssize_t nread, const uv_buf_t *buf)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) stream, ICProxyPeer, tcp);

	if (unlikely(nread < 0))
	{
		if (nread != UV_EOF)
			elog(WARNING, "ic-proxy: %s: failed to recv HELLO ACK: %s",
						 peer->name, uv_strerror(nread));
		else
			elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
				   "ic-proxy: %s: received EOF while waiting for HELLO ACK",
						 peer->name);

		if (buf->base)
			ic_proxy_pkt_cache_free(buf->base);

		ic_proxy_peer_shutdown(peer);
		return;
	}
	else if (unlikely(nread == 0))
	{
		if (buf->base)
			ic_proxy_pkt_cache_free(buf->base);

		/* EAGAIN or EWOULDBLOCK, retry */
		return;
	}

	ic_proxy_ibuf_push(&peer->ibuf, buf->base, nread,
					   ic_proxy_peer_on_hello_ack_pkt, peer);
	ic_proxy_pkt_cache_free(buf->base);
}

/*
 * Sent the HELLO message.
 */
static void
ic_proxy_peer_on_sent_hello(void *opaque, const ICProxyPkt *pkt, int status)
{
	ICProxyPeer *peer = opaque;

	if (status < 0)
	{
		ic_proxy_peer_shutdown(peer);
		return;
	}

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG1,
		   "ic-proxy: %s: waiting for HELLO ACK", peer->name);

	peer->state |= IC_PROXY_PEER_STATE_SENT_HELLO;

	peer->state |= IC_PROXY_PEER_STATE_RECEIVING_HELLO_ACK;

	/* wait for hello ack */
	ic_proxy_peer_uv_read_start(peer, ic_proxy_peer_on_hello_ack_data);
}

/*
 * Connected to a peer.
 */
static void
ic_proxy_peer_on_connected(uv_connect_t *conn, int status)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) conn->handle, ICProxyPeer, tcp);

	ic_proxy_free(conn);

	if (status < 0)
	{
		/* the peer might just not get ready yet, retry later */
		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
			   "ic-proxy: %s: failed to connect: %s",
					 peer->name, uv_strerror(status));
		ic_proxy_peer_close(peer);
		return;
	}

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG1,
		   "ic-proxy: %s: connected, sending HELLO", peer->name);

	peer->state |= IC_PROXY_PEER_STATE_CONNECTED;

	/* TODO: increase ic_proxy_peer_contents[peer->content] */

	ic_proxy_peer_update_name(peer);

	/*
	 * If TLS is enabled, hand the fd off to a uv_poll_t-driven path
	 * (see ic_proxy_peer_tls_start). The HELLO message rides inside
	 * the TLS channel; the post-handshake dispatcher eventually
	 * calls ic_proxy_peer_client_send_hello.
	 */
	if (ic_proxy_tls_is_enabled())
	{
		ic_proxy_peer_tls_start(peer, false /* is_server */);
		return;
	}

	ic_proxy_peer_client_send_hello(peer);
}

/*
 * Build the HELLO message and send it. Called from
 * ic_proxy_peer_on_connected directly on the plaintext path, and from
 * the TLS post-handshake dispatcher once the TLS channel is up.
 */
static void
ic_proxy_peer_client_send_hello(ICProxyPeer *peer)
{
	ICProxyKey	key;

	/* hello packet must be the first one from a client */

	/*
	 * For a peer HELLO message, the only meaningful field is localDbid,
	 * but we also set the content and pid for debugging purpose.
	 */
	ic_proxy_key_init(&key,
					  0						/* sessionId */,
					  0						/* commandId */,
					  0						/* sendSliceIndex */,
					  0						/* recvSliceIndex */,
					  GpIdentity.segindex	/* localContentId */,
					  GpIdentity.dbid		/* localDbid */,
					  MyProcPid				/* localPid */,
					  peer->content			/* remoteContentId */,
					  peer->dbid			/* remoteDbid */,
					  0						/* remotePid */);

	peer->state |= IC_PROXY_PEER_STATE_SENDING_HELLO;

	ic_proxy_peer_send_message(peer, IC_PROXY_MESSAGE_PEER_HELLO, &key,
							   ic_proxy_peer_on_sent_hello);
}

/*
 * Connect to a remote peer.
 */
void
ic_proxy_peer_connect(ICProxyPeer *peer, struct sockaddr_in *dest)
{
	uv_connect_t *conn;
	char		name[HOST_NAME_MAX];

	if (peer->state & IC_PROXY_PEER_STATE_CONNECTING)
		return;

	peer->state |= IC_PROXY_PEER_STATE_CONNECTING;

	uv_ip4_name(dest, name, sizeof(name));
	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy: %s: connecting to %s:%d",
				 peer->name, name, ntohs(dest->sin_port));

	/* reinit the tcp handle */
	uv_tcp_init(peer->tcp.loop, &peer->tcp);
	uv_tcp_nodelay(&peer->tcp, true);

	conn = ic_proxy_new(uv_connect_t);

	uv_tcp_connect(conn, &peer->tcp, (struct sockaddr *) dest,
				   ic_proxy_peer_on_connected);
}

/*
 * Disconnect a peer.
 *
 * The peer can be in any state, the caller only needs to ensure not to call
 * this function from a peer callback.
 */
void
ic_proxy_peer_disconnect(ICProxyPeer *peer)
{
	/* No such a peer yet */
	if (!peer)
		return;

	/* No connection is made or being made */
	if (!(peer->state & IC_PROXY_PEER_STATE_CONNECTING))
		return;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy: %s: disconnecting", peer->name);
	ic_proxy_peer_shutdown(peer);
}

/*
 * Send a packet to a remote peer.
 */
void
ic_proxy_peer_route_data(ICProxyPeer *peer, ICProxyPkt *pkt,
						 ic_proxy_sent_cb callback, void *opaque)
{
	if (!(peer->state & IC_PROXY_PEER_STATE_READY_FOR_DATA))
	{
		ICProxyDelay *delay;

		elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
			   "ic-proxy: %s: caching outgoing %s",
					 peer->name, ic_proxy_pkt_to_str(pkt));

		delay = ic_proxy_peer_build_delay(peer, pkt, callback, opaque);
		peer->reqs = lappend(peer->reqs, delay);

		return;
	}

	ic_proxy_router_write((uv_stream_t *) &peer->tcp, pkt, 0, callback, opaque);
}

/*
 * Send the peer control message, HELLO and HELLO ACK.  The client control
 * message should be sent with ic_proxy_peer_route_data().
 *
 * TODO: it's better to separate the peer messages from the client messages
 * completely.
 */
static void
ic_proxy_peer_send_message(ICProxyPeer *peer, ICProxyMessageType mtype,
						   const ICProxyKey *key, ic_proxy_sent_cb callback)
{
	ICProxyPkt *pkt;

	if (!(peer->state & IC_PROXY_PEER_STATE_READY_FOR_MESSAGE))
		elog(ERROR,
					 "ic-proxy: %s: not ready to send or receive messages",
					 peer->name);

	pkt = ic_proxy_message_new(mtype, key);

	ic_proxy_router_write((uv_stream_t *) &peer->tcp, pkt, 0, callback, peer);
}

/*
 * This function is only called on a new peer, so it is not so expansive to
 * rebuild the cache list.
 */
static void
ic_proxy_peer_handle_out_cache(ICProxyPeer *peer)
{
	List	   *reqs;
	ListCell   *cell;

	if (!(peer->state & IC_PROXY_PEER_STATE_READY_FOR_DATA))
		return;

	if (peer->reqs == NIL)
		return;

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy: %s: trying to consume the %d cached outgoing pkts",
				 peer->name, list_length(peer->reqs));

	/* First detach all the pkts */
	reqs = peer->reqs;
	peer->reqs = NIL;

	/* Then re-handle them one by one */
	foreach(cell, reqs)
	{
		ICProxyDelay *delay = lfirst(cell);

		/* TODO: can we pass the delay directly? */
		ic_proxy_peer_route_data(peer, delay->pkt,
								 delay->callback, delay->opaque);

		ic_proxy_free(delay);
	}

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG, DEBUG3,
		   "ic-proxy: %s: consumed %d cached pkts",
				 peer->name, list_length(reqs) - list_length(peer->reqs));

	/*
	 * the pkts ownership were transfered during ic_proxy_peer_route_data(),
	 * only need to free the list itself.
	 */
	list_free(reqs);
}


/*
 * ============================================================
 * TLS plumbing — uv_poll_t + socket BIO bridge.
 * ============================================================
 *
 * Lifecycle of a TLS-enabled peer:
 *
 *   1. Standard libuv connect/accept on peer->tcp (uv_tcp_t) gives
 *      us a connected fd.
 *
 *   2. ic_proxy_peer_tls_start: dup() the fd out of peer->tcp, then
 *      uv_close(&peer->tcp). The dup'd fd is stored as peer->tls_fd;
 *      this releases libuv's epoll registration on the original fd
 *      so we can re-register our own uv_poll_t on the dup.
 *
 *   3. uv_close callback ic_proxy_peer_tls_on_tcp_closed_*: by now
 *      peer->tcp is gone. Initialize peer->tls_poll on tls_fd,
 *      allocate the ICProxyTlsConn (which creates the OpenSSL SSL
 *      + socket BIO over tls_fd), and kick the handshake step.
 *
 *   4. ic_proxy_peer_tls_step_handshake: call SSL_do_handshake; based
 *      on WANT_READ / WANT_WRITE / DONE, arm peer->tls_poll for the
 *      appropriate events or dispatch into the data plane.
 *
 *   5. After handshake DONE, dispatch by role:
 *      - server: ic_proxy_peer_read_hello — uv_read_start wrapper
 *        sees peer->tls != NULL and just arms tls_poll for read;
 *        SSL_read drain delivers plaintext into on_hello_data;
 *      - client: ic_proxy_peer_client_send_hello — packets queued
 *        via ic_proxy_router_write are picked up by
 *        ic_proxy_peer_tls_queue_write and drained on UV_WRITABLE
 *        events via SSL_write.
 *
 *   6. Data plane: ic_proxy_peer_tls_on_poll keeps cycling — read
 *      events drain SSL_read into pkt-cache buffers (forwarded to
 *      peer->tls_real_read_cb); write events drain
 *      peer->tls_tx_queue via SSL_write; the desired event mask is
 *      recomputed at the end of each cycle.
 *
 * When the kernel supports kTLS AND the negotiated cipher matches
 * the kernel's tls module, OpenSSL's handshake completion uploads
 * keys via setsockopt under the hood (SSL_OP_ENABLE_KTLS path).
 * After upload, SSL_read / SSL_write become wrappers over kernel
 * sendmsg/recvmsg — AEAD runs in the kernel (or in NIC if it has
 * TLS offload). When kTLS is not available, OpenSSL transparently
 * stays on userspace AEAD; same socket BIO, same on-wire records,
 * only the cost of crypto differs.
 *
 * BIO_get_ktls_send/recv is checked inside
 * ic_proxy_tls_conn_handshake_step and the result logged for ops
 * visibility (see "kTLS tx=on rx=on" in the bgworker log).
 */

/*
 * One outbound packet awaiting SSL_write. Encrypted-record framing
 * is owned by OpenSSL; we just track plaintext send progress.
 */
typedef struct ICProxyTlsTxItem
{
	ICProxyPkt *pkt;
	int32		offset;				/* offset within pkt to start from */
	int32		sent;				/* how many plaintext bytes already
									 * accepted by SSL_write */
	ic_proxy_sent_cb callback;
	void	   *opaque;
} ICProxyTlsTxItem;

/* Cap on the per-peer freelist size. Each item is ~32 B so 64 caps at
 * ~2 KB / peer — negligible compared to the 64 KB tls_tx_scratch. */
#define IC_PROXY_TLS_TX_ITEM_FREELIST_CAP	64

/*
 * Pull an ICProxyTlsTxItem from the per-peer freelist or palloc one.
 * The hot path on a busy data plane is the pop branch — one less
 * MemoryContext round-trip per motion packet.
 */
static ICProxyTlsTxItem *
ic_proxy_peer_tls_tx_item_get(ICProxyPeer *peer)
{
	ICProxyTlsTxItem *item;

	if (peer->tls_tx_item_freelist != NIL)
	{
		item = (ICProxyTlsTxItem *) linitial(peer->tls_tx_item_freelist);
		peer->tls_tx_item_freelist = list_delete_first(peer->tls_tx_item_freelist);
		return item;
	}
	return ic_proxy_new(ICProxyTlsTxItem);
}

/*
 * Fail every packet still queued for SSL_write: invoke its callback
 * with the given status, return its pkt-cache buffer, and free the
 * item.  Must run on every teardown path — the queue items hold
 * buffers from the fixed-size packet cache, so leaking them across
 * repeated peer churn would exhaust the pool.
 */
static void
ic_proxy_peer_tls_fail_tx_queue(ICProxyPeer *peer, int status)
{
	List	   *queue = peer->tls_tx_queue;
	ListCell   *cell;

	/* detach first: a callback may route more packets at this peer */
	peer->tls_tx_queue = NIL;

	foreach(cell, queue)
	{
		ICProxyTlsTxItem *item = (ICProxyTlsTxItem *) lfirst(cell);

		if (item->callback != NULL)
			item->callback(item->opaque, item->pkt, status);
		ic_proxy_pkt_cache_free(item->pkt);
		ic_proxy_free(item);
	}
	list_free(queue);
}

/*
 * Return a fully-consumed item to the freelist, or pfree it when the
 * freelist is already at cap. The cap keeps an idle peer from
 * pinning unbounded memory after a traffic spike.
 */
static void
ic_proxy_peer_tls_tx_item_put(ICProxyPeer *peer, ICProxyTlsTxItem *item)
{
	if (list_length(peer->tls_tx_item_freelist)
		>= IC_PROXY_TLS_TX_ITEM_FREELIST_CAP)
	{
		ic_proxy_free(item);
		return;
	}
	/* lcons (push-front) is O(1) — no need to keep order. */
	peer->tls_tx_item_freelist = lcons(item, peer->tls_tx_item_freelist);
}


/*
 * Step 1+2: kick off the migration from uv_tcp_t to uv_poll_t. Pulls
 * the fd out of peer->tcp, uv_close's the tcp handle, and registers
 * the role-specific tcp-closed callback that continues setup once
 * libuv has finalised the close.
 */
static void
ic_proxy_peer_tls_start(ICProxyPeer *peer, bool is_server)
{
	int			fd = -1;
	int			ret;
	int			dup_fd;

	ret = uv_fileno((uv_handle_t *) &peer->tcp, &fd);
	if (ret != 0)
	{
		elog(WARNING, "ic-proxy TLS: %s: uv_fileno failed: %s",
			 peer->name, uv_strerror(ret));
		ic_proxy_peer_close(peer);
		return;
	}

	dup_fd = dup(fd);
	if (dup_fd < 0)
	{
		elog(WARNING, "ic-proxy TLS: %s: dup failed: %m", peer->name);
		ic_proxy_peer_close(peer);
		return;
	}

	peer->tls_fd = dup_fd;

	/*
	 * uv_close releases the underlying epoll registration and
	 * closes the original fd. After the callback runs, peer->tcp
	 * is no longer in use and peer->tls_fd is ours to register on
	 * a uv_poll_t.
	 */
	uv_close((uv_handle_t *) &peer->tcp,
			 is_server ? ic_proxy_peer_tls_on_tcp_closed_for_server
					   : ic_proxy_peer_tls_on_tcp_closed_for_client);
}

static void
ic_proxy_peer_tls_on_tcp_closed_for_client(uv_handle_t *handle)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) handle, ICProxyPeer, tcp);

	ic_proxy_peer_tls_after_close(peer, false /* is_server */);
}

static void
ic_proxy_peer_tls_on_tcp_closed_for_server(uv_handle_t *handle)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) handle, ICProxyPeer, tcp);

	ic_proxy_peer_tls_after_close(peer, true /* is_server */);
}

/*
 * Step 3: tcp handle is closed; bring up the poll handle + SSL state.
 */
static void
ic_proxy_peer_tls_after_close(ICProxyPeer *peer, bool is_server)
{
	int			ret;

	/*
	 * Closing in flight (e.g. peer_close was called before this
	 * callback fired). Tear our piece down.
	 */
	if (peer->state & IC_PROXY_PEER_STATE_CLOSING)
	{
		if (peer->tls_fd >= 0)
		{
			close(peer->tls_fd);
			peer->tls_fd = -1;
		}
		return;
	}

	peer->tls = ic_proxy_tls_conn_new(peer->tls_fd, is_server);
	Assert(peer->tls != NULL);

	ret = uv_poll_init_socket(peer->tcp.loop, &peer->tls_poll, peer->tls_fd);
	if (ret != 0)
	{
		elog(WARNING, "ic-proxy TLS: %s: uv_poll_init_socket failed: %s",
			 peer->name, uv_strerror(ret));
		ic_proxy_tls_conn_free(peer->tls);
		peer->tls = NULL;
		close(peer->tls_fd);
		peer->tls_fd = -1;
		ic_proxy_peer_unregister(peer);
		return;
	}

	elogif(gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE, LOG,
		   "ic-proxy TLS: %s: started %s handshake (fd=%d)", peer->name,
		   is_server ? "server" : "client", peer->tls_fd);

	/* Kick the handshake; subsequent progress is driven by poll events. */
	ic_proxy_peer_tls_step_handshake(peer);
}

/*
 * Step 4: SSL_do_handshake + arm poll events / dispatch on done.
 */
static void
ic_proxy_peer_tls_step_handshake(ICProxyPeer *peer)
{
	ICProxyTlsHandshakeResult r = ic_proxy_tls_conn_handshake_step(peer->tls);

	if (r == IC_PROXY_TLS_HS_FATAL)
	{
		ic_proxy_peer_close(peer);
		return;
	}

	if (r == IC_PROXY_TLS_HS_DONE)
	{
		ic_proxy_peer_tls_post_handshake(peer);
		return;
	}

	{
		int			events = (r == IC_PROXY_TLS_HS_WANT_READ) ? UV_READABLE
															  : UV_WRITABLE;

		ic_proxy_peer_tls_poll_arm(peer, events);
	}
}

/*
 * Step 5: handshake done. Resume the role-specific HELLO path.
 */
static void
ic_proxy_peer_tls_post_handshake(ICProxyPeer *peer)
{
	if (peer->state & IC_PROXY_PEER_STATE_ACCEPTED)
	{
		/*
		 * Server side: arm read for HELLO. ic_proxy_peer_read_hello
		 * sees peer->tls != NULL and calls our uv_read_start wrapper,
		 * which records the cb in peer->tls_real_read_cb and re-arms
		 * tls_poll for UV_READABLE.
		 */
		ic_proxy_peer_read_hello(peer);
	}
	else
	{
		Assert(peer->state & IC_PROXY_PEER_STATE_CONNECTED);
		ic_proxy_peer_client_send_hello(peer);
	}

	/*
	 * After the role-specific dispatch the peer has either armed a
	 * read (server) or queued a write (client). Recompute the poll
	 * mask either way so the next event delivers correctly.
	 */
	if (peer->tls != NULL)
		ic_proxy_peer_tls_rearm_poll(peer);
}

/*
 * Read SSL_read in a loop, deliver each plaintext chunk to the
 * caller-registered cb (peer->tls_real_read_cb) as if from uv_read.
 *
 * cb's signature is uv_read_cb (uv_stream_t *, ssize_t, const uv_buf_t *).
 * We pass peer->tcp as the stream — even though it's been uv_close'd,
 * the memory is still valid: the cbs recover the peer with
 * CONTAINER_OF(stream, ICProxyPeer, tcp) (pointer math) and read
 * peer->tcp.loop, which uv_close leaves intact.
 *
 * tls_real_read_cb is re-read on every iteration: a cb may switch the
 * reader (hello → data handoff) or stop reading altogether via
 * ic_proxy_peer_uv_read_stop, and later chunks must honor that.
 */
static void
ic_proxy_peer_tls_drain_rx(ICProxyPeer *peer)
{
	while (true)
	{
		uv_read_cb	cb = peer->tls_real_read_cb;
		size_t		plain_size;
		char	   *plain;
		int			n;
		int			out_event = 0;
		uv_buf_t	plain_buf;

		if (cb == NULL)
			return;				/* nobody asked us to read (anymore) */

		plain = ic_proxy_pkt_cache_alloc(&plain_size);

		n = ic_proxy_tls_conn_read(peer->tls, plain, (int) plain_size,
								   &out_event);
		if (n > 0)
		{
			plain_buf.base = plain;
			plain_buf.len = plain_size;
			cb((uv_stream_t *) &peer->tcp, (ssize_t) n, &plain_buf);
			/* cb owns the buffer now */
			continue;
		}

		ic_proxy_pkt_cache_free(plain);

		if (n == 0)
		{
			/*
			 * WANT_READ — rearm_poll keeps UV_READABLE armed while the
			 * read cb exists.  (A cross-direction WANT_WRITE is only
			 * possible on the userspace-AEAD path during a KeyUpdate
			 * and resolves on the next poll cycle; see out_event's
			 * contract in ic_proxy_tls.h.)
			 */
			break;
		}

		/* fatal — signal EOF to the cb */
		{
			uv_buf_t	empty = { NULL, 0 };

			cb((uv_stream_t *) &peer->tcp, UV_EOF, &empty);
		}
		break;
	}
}

/*
 * Coalesce up to IC_PROXY_TLS_TX_COALESCE_MAX bytes from the head of
 * peer->tls_tx_queue into one scratch buffer, then SSL_write the
 * whole thing in a single call. After the SSL_write returns N, walk
 * back through the queue items and apportion N bytes — popping any
 * item that became fully sent.
 *
 * Without coalescing every motion packet (a few hundred bytes to
 * 8 KB) became its own SSL_write call and therefore its own TLS
 * record, paying ~30 bytes of fixed overhead (13-byte header +
 * 16-byte AEAD tag + 1-byte content type) per record AND one kernel
 * sendmsg syscall. With coalescing a queue burst lands in one or
 * two 16 KB TLS records and the corresponding sendmsg count drops
 * proportionally; the per-record overhead amortises to ~0.2 %.
 *
 * SSL_MODE_ENABLE_PARTIAL_WRITE + SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
 * were set in ic_proxy_tls_conn_new; without those OpenSSL would
 * require us to retry SSL_write with the same buffer + length on a
 * WANT_WRITE, which fights this coalescing pattern.
 *
 * The scratch buffer is a stack local; libuv is single-threaded so
 * we don't need it to outlive this function.
 */
#define IC_PROXY_TLS_TX_COALESCE_MAX	(64 * 1024)

static void
ic_proxy_peer_tls_drain_tx(ICProxyPeer *peer)
{
	int			cork_on = 1;
	int			cork_off = 0;
	bool		corked = false;

	/*
	 * Per-peer scratch reused across drains so its physical pages
	 * stay warm in L1d / L2. Lazy-alloc on first drain — peers that
	 * never write (e.g. a pure receiver of a Gather motion) don't
	 * pay the 64 KB heap cost.
	 */
	if (peer->tls_tx_scratch == NULL)
		peer->tls_tx_scratch = ic_proxy_alloc(IC_PROXY_TLS_TX_COALESCE_MAX);

	while (peer->tls_tx_queue != NIL)
	{
		char	   *scratch = peer->tls_tx_scratch;
		int			total = 0;
		ListCell   *cell;
		int			out_event = 0;
		int			n;

		/*
		 * Phase 1: fill scratch from the queue front, packet by
		 * packet. item->(offset, sent) tracks how far we've already
		 * pushed past its start; we resume at item->sent so a
		 * previous partial write picks up correctly here.
		 */
		foreach(cell, peer->tls_tx_queue)
		{
			ICProxyTlsTxItem *item = (ICProxyTlsTxItem *) lfirst(cell);
			int			remaining = (int) item->pkt->len - item->offset - item->sent;
			int			free_space = IC_PROXY_TLS_TX_COALESCE_MAX - total;
			int			copy = remaining < free_space ? remaining : free_space;

			memcpy(scratch + total,
				   ((const char *) item->pkt) + item->offset + item->sent,
				   copy);
			total += copy;

			if (copy < remaining)
				break;				/* scratch full mid-item */
			if (total == IC_PROXY_TLS_TX_COALESCE_MAX)
				break;				/* scratch full at item boundary */
		}

		if (total == 0)
			break;					/* shouldn't happen but bail safely */

		/*
		 * About to ship records. Cork the socket the first time we
		 * have anything to flush so the kernel coalesces consecutive
		 * sendmsg calls (one per TLS record, when kTLS is engaged)
		 * into larger TCP segments. Each TLS-1.3 record caps at
		 * ~16 KB and our 64 KB scratch can produce up to 4 records
		 * per SSL_write; without TCP_CORK those go out as 4 separate
		 * TCP segments because we set TCP_NODELAY on the listener.
		 *
		 * Lazy-cork (vs cork-at-function-entry) saves the 2-syscall
		 * overhead for drains that fill the scratch with 0 bytes
		 * (queue head is in mid-partial-write recovery).
		 */
		if (!corked)
		{
			setsockopt(peer->tls_fd, IPPROTO_TCP, TCP_CORK,
					   &cork_on, sizeof(cork_on));
			corked = true;
		}

		/*
		 * Phase 2: ship the coalesced buffer. SSL_write may return
		 * fewer bytes than total under partial-write mode; we'll
		 * apportion whatever it accepted across queue items in
		 * Phase 3.
		 */
		n = ic_proxy_tls_conn_write(peer->tls, scratch, total, &out_event);
		if (n > 0)
		{
			int			consumed = n;

			/*
			 * Phase 3: advance item->sent counters and pop items
			 * whose entire payload made it into SSL.
			 */
			while (consumed > 0 && peer->tls_tx_queue != NIL)
			{
				ICProxyTlsTxItem *item = (ICProxyTlsTxItem *) linitial(peer->tls_tx_queue);
				int			item_remaining = (int) item->pkt->len - item->offset - item->sent;
				int			take = consumed < item_remaining ? consumed : item_remaining;

				item->sent += take;
				consumed -= take;

				if (item->sent + item->offset >= (int32) item->pkt->len)
				{
					peer->tls_tx_queue = list_delete_first(peer->tls_tx_queue);
					if (item->callback != NULL)
						item->callback(item->opaque, item->pkt, 0);
					ic_proxy_pkt_cache_free(item->pkt);
					ic_proxy_peer_tls_tx_item_put(peer, item);
				}
			}
			continue;
		}

		if (n == 0)
		{
			/*
			 * WANT_WRITE — rearm_poll keeps UV_WRITABLE armed while
			 * the queue is non-empty.  (A cross-direction WANT_READ
			 * resolves on the next readable cycle; see out_event's
			 * contract in ic_proxy_tls.h.)
			 */
			break;
		}

		/* fatal — fail this item and the rest of the queue */
		ic_proxy_peer_tls_fail_tx_queue(peer, UV_ECONNRESET);

		if (corked)
			setsockopt(peer->tls_fd, IPPROTO_TCP, TCP_CORK,
					   &cork_off, sizeof(cork_off));
		ic_proxy_peer_close(peer);
		return;
	}

	/*
	 * Release TCP_CORK so the queued ciphertext flushes out as
	 * coalesced TCP segments instead of waiting for more sendmsg's
	 * that may not come.
	 */
	if (corked)
		setsockopt(peer->tls_fd, IPPROTO_TCP, TCP_CORK,
				   &cork_off, sizeof(cork_off));
}

/*
 * Apply an event mask to peer->tls_poll, skipping the libuv call if
 * the mask is already what we want. uv_poll_start invokes
 * epoll_ctl(EPOLL_CTL_MOD) internally; on a busy data plane (queue
 * stays non-empty, read still desired across drain cycles) the
 * mask we compute often matches what's already armed and the
 * epoll_ctl is wasted. Cache the last-armed mask in
 * peer->tls_poll_armed_events and short-circuit on match.
 *
 * 0 means "stopped".
 */
static void
ic_proxy_peer_tls_poll_arm(ICProxyPeer *peer, int events)
{
	int			ret;

	if (events == peer->tls_poll_armed_events)
		return;

	if (events == 0)
	{
		uv_poll_stop(&peer->tls_poll);
		peer->tls_poll_armed_events = 0;
		return;
	}

	ret = uv_poll_start(&peer->tls_poll, events,
						ic_proxy_peer_tls_on_poll);
	if (ret != 0)
	{
		elog(WARNING, "ic-proxy TLS: %s: uv_poll_start failed: %s",
			 peer->name, uv_strerror(ret));
		ic_proxy_peer_close(peer);
		return;
	}
	peer->tls_poll_armed_events = events;
}

/*
 * Recompute the desired event mask from peer state and apply it.
 *
 * We always want UV_READABLE while peer->tls_real_read_cb is armed
 * (so SSL can decrypt incoming records); UV_WRITABLE is added when
 * there are pending tx items. UV_DISCONNECT is implicit through
 * SSL_ERROR_ZERO_RETURN handling in the drain loops.
 */
static void
ic_proxy_peer_tls_rearm_poll(ICProxyPeer *peer)
{
	int			events = 0;

	/* tls_poll is stopping or already stopped once CLOSING is set */
	if (peer->tls == NULL || (peer->state & IC_PROXY_PEER_STATE_CLOSING))
		return;

	if (peer->tls_real_read_cb != NULL)
		events |= UV_READABLE;
	if (peer->tls_tx_queue != NIL)
		events |= UV_WRITABLE;

	ic_proxy_peer_tls_poll_arm(peer, events);
}

/*
 * Step 6: poll event handler. Drives handshake or data plane.
 */
static void
ic_proxy_peer_tls_on_poll(uv_poll_t *handle, int status, int events)
{
	ICProxyPeer *peer = CONTAINER_OF((void *) handle, ICProxyPeer, tls_poll);

	if (status < 0)
	{
		elog(WARNING, "ic-proxy TLS: %s: poll status %d: %s",
			 peer->name, status, uv_strerror(status));
		ic_proxy_peer_close(peer);
		return;
	}

	if (!ic_proxy_tls_conn_handshake_done(peer->tls))
	{
		ic_proxy_peer_tls_step_handshake(peer);
		return;
	}

	/*
	 * Data plane. Service writes first so backlogged outgoing data
	 * doesn't sit while we drain reads; either drain may turn
	 * EAGAIN at any moment and we re-arm at the end.
	 */
	if (events & UV_WRITABLE)
		ic_proxy_peer_tls_drain_tx(peer);
	if (events & UV_READABLE)
		ic_proxy_peer_tls_drain_rx(peer);

	if (peer->tls != NULL)
		ic_proxy_peer_tls_rearm_poll(peer);
}

/*
 * Queue an outgoing packet on a TLS peer. Called from
 * ic_proxy_router_write when it detects the destination is a
 * TLS-active peer. Takes ownership of pkt.
 */
void
ic_proxy_peer_tls_queue_write(ICProxyPeer *peer, ICProxyPkt *pkt, int32 offset,
							  ic_proxy_sent_cb callback, void *opaque)
{
	ICProxyTlsTxItem *item;

	Assert(peer->tls != NULL);
	Assert(ic_proxy_tls_conn_handshake_done(peer->tls));

	item = ic_proxy_peer_tls_tx_item_get(peer);
	item->pkt = pkt;
	item->offset = offset;
	item->sent = 0;
	item->callback = callback;
	item->opaque = opaque;

	peer->tls_tx_queue = lappend(peer->tls_tx_queue, item);

	/*
	 * Re-arm for UV_WRITABLE now. We could try a drain inline first,
	 * but doing it from the next poll cycle keeps the call stack
	 * simple and avoids reentrancy through the sent callback.
	 */
	ic_proxy_peer_tls_rearm_poll(peer);
}
