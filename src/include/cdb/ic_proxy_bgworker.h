/*-------------------------------------------------------------------------
 *
 * ic_proxy_bgworker.h
 *	  TODO file description
 *
 *
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates.
 *
 *
 *-------------------------------------------------------------------------
 */

#ifndef IC_PROXY_BGWORKER_H
#define IC_PROXY_BGWORKER_H

#include "port/atomics.h"

/*
 * Flag (in SHM) telling the backends why the ic-proxy process cannot serve
 * them, so that a proxy-mode query fails at interconnect setup instead of
 * retrying the connection to the proxy until gp_interconnect_setup_timeout.
 */
#define IC_PROXY_FAILURE_NONE			0
#define IC_PROXY_FAILURE_LISTEN			1	/* peer listener bind / listen failed,
											 * retried every second */
#define IC_PROXY_FAILURE_TLS_CONFIG		2	/* TLS settings invalid, the process
											 * exited and stays down until the
											 * postmaster restarts */
extern pg_atomic_uint32 *ic_proxy_peer_listener_failed;

extern bool ICProxyStartRule(Datum main_arg);
extern void ICProxyMain(Datum main_arg);
extern Size ICProxyShmemSize(void);
extern void ICProxyShmemInit(void);

#endif   /* IC_PROXY_BGWORKER_H */
