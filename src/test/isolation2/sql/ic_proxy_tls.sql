-- TLS on the ic-proxy peer connections: the data plane, the teardown paths
-- and the configuration fail-stop.
--
-- The schedule assumes gp_interconnect_type=proxy with a configured mesh.
-- TLS is switched on for the duration of this test and switched off again
-- at the end, so the cluster is left the way the schedule expects it.
-- Every restart kills the open sessions, hence the numbered sessions that
-- quit before each restart.
--
-- gp_toolkit.gp_log_system reads the log of every segment; that is how the
-- test tells that the peers really negotiated TLS and that the teardown
-- paths stayed quiet.  Log lines older than the marker taken at the start
-- of the test are ignored.

1: CREATE TABLE ic_proxy_tls_t AS SELECT i FROM generate_series(1, 10000) i DISTRIBUTED BY (i);
1: CREATE TABLE ic_proxy_tls_marker AS SELECT now() AS t DISTRIBUTED REPLICATED;
1q:

-- Switch TLS on (PGC_POSTMASTER: needs a restart).
-- start_ignore
!\retcode gpconfig -c gp_interconnect_proxy_tls_enable -v on;
!\retcode gpstop -ari;
-- end_ignore

-- Motion through the TLS mesh.
2: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;

-- The peers completed TLS 1.3 handshakes with the pinned cipher suite.
2: SELECT count(*) >= 1 AS handshakes_logged
   FROM gp_toolkit.gp_log_system
   WHERE logtime > (SELECT t FROM ic_proxy_tls_marker)
   AND logmessage LIKE 'ic-proxy TLS: %: handshake complete (role=server, cipher=TLS_AES_256_GCM_SHA384)';

-- Peer teardown under TLS: kill one segment's proxy.  The postmaster
-- restarts it, the other proxies drop the peer, and the next query goes
-- through again.
!\retcode ps -ef | grep $(psql postgres -Atc "select port from gp_segment_configuration where content=1 and role='p'") | grep "ic proxy" | awk '{print $2}' | xargs kill;
!\retcode sleep 3;
2: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;

-- The killed proxy's connections ended with a bare TCP close; the remote
-- proxies must read that as EOF, not as a TLS read failure.
2: SELECT count(*) AS tls_warnings
   FROM gp_toolkit.gp_log_system
   WHERE logtime > (SELECT t FROM ic_proxy_tls_marker)
   AND logseverity = 'WARNING'
   AND logmessage LIKE 'ic-proxy TLS:%';

-- Misconfiguration: a CA file without our own certificate can never verify,
-- so the proxy refuses to start.  It exits once (no restart loop) and a
-- proxy-mode query fails at interconnect setup instead of hanging.
2q:
-- start_ignore
!\retcode gpconfig -c gp_interconnect_proxy_tls_ca_file -v /nonexistent/ca.pem;
!\retcode gpstop -ari;
-- end_ignore
3: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;

-- With the proxies down nothing distributed can run, so look at the
-- coordinator's own log (a coordinator-only foreign table, no motion); the
-- segments went through the same code.  Everything since this postmaster
-- started: one proxy start, one fail-stop, the configuration ERROR.
3: SELECT count(*) AS proxy_starts
   FROM gp_toolkit.__gp_log_coordinator_ext
   WHERE logtime >= pg_postmaster_start_time()
   AND logmessage = 'ic-proxy: server setting up';
3: SELECT count(*) AS fail_stops
   FROM gp_toolkit.__gp_log_coordinator_ext
   WHERE logtime >= pg_postmaster_start_time()
   AND logmessage LIKE 'ic-proxy: TLS configuration is invalid%';
3: SELECT count(*) AS config_errors
   FROM gp_toolkit.__gp_log_coordinator_ext
   WHERE logtime >= pg_postmaster_start_time()
   AND logseverity = 'ERROR'
   AND logmessage LIKE 'ic-proxy TLS: gp_interconnect_proxy_tls_ca_file requires%';
3q:

-- Back to plain TCP peers.
-- start_ignore
!\retcode gpconfig -r gp_interconnect_proxy_tls_ca_file;
!\retcode gpconfig -c gp_interconnect_proxy_tls_enable -v off;
!\retcode gpstop -ari;
-- end_ignore
4: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;

4: DROP TABLE ic_proxy_tls_t;
4: DROP TABLE ic_proxy_tls_marker;
4q:
