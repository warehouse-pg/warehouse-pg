-- TLS on the ic-proxy peer connections: the data plane, the teardown paths,
-- mutual verification, the reload of the certificates and the handling of
-- unusable settings.
--
-- The schedule assumes gp_interconnect_type=proxy with a configured mesh.
-- TLS is switched on for the duration of this test and switched off again
-- at the end, so the cluster is left the way the schedule expects it.
-- Every restart kills the open sessions, hence the numbered sessions that
-- quit before each restart.
--
-- What the proxies do is observed from the outside wherever that is
-- possible: "openssl s_client" talks to the coordinator's proxy port and
-- its exit status says whether the proxy negotiated what it should and
-- refused what it should, and the pid of a proxy process says whether it
-- was restarted.  Only what must not happen is looked up in the log: a
-- teardown or a refused stranger must not make the proxies report trouble
-- on their healthy connections.  Only the log lines of this run count,
-- those after the marker line the test writes to the coordinator's log
-- first; logtime is only compared with logtime, never with a clock, since
-- the log's timestamps carry a time zone abbreviation that may not read
-- back as the zone it was written in (CST of Asia/Shanghai reads back as
-- US Central).
--
-- The certificates are made by the openssl command in /tmp/ic_proxy_tls_gen
-- and what the proxies need of them is copied to /tmp/ic_proxy_tls_test on
-- every host: a CA, one certificate of that CA for all the proxies (the
-- verification does not bind a certificate to a host), and a self-signed
-- "rogue" certificate that the CA did not issue.

1: CREATE TABLE ic_proxy_tls_t AS SELECT i FROM generate_series(1, 10000) i DISTRIBUTED BY (i);
1: DO $$ BEGIN RAISE LOG 'ic_proxy_tls: test start'; END $$;
1q:

!\retcode (rm -rf /tmp/ic_proxy_tls_gen && mkdir /tmp/ic_proxy_tls_gen && cd /tmp/ic_proxy_tls_gen && openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -subj "/CN=ic-proxy-test-ca" -days 2 && openssl req -newkey rsa:2048 -nodes -keyout peer.key -out peer.csr -subj "/CN=ic-proxy-test-peer" && openssl x509 -req -in peer.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out peer.crt -days 2 && openssl req -x509 -newkey rsa:2048 -nodes -keyout rogue.key -out rogue.crt -subj "/CN=ic-proxy-test-rogue" -days 2 && chmod 600 *.key) >/dev/null 2>&1;
!\retcode for h in $(psql postgres -Atc "select distinct hostname from gp_segment_configuration"); do ssh -n $h "rm -rf /tmp/ic_proxy_tls_test && mkdir /tmp/ic_proxy_tls_test" && scp -pq /tmp/ic_proxy_tls_gen/ca.crt /tmp/ic_proxy_tls_gen/peer.crt /tmp/ic_proxy_tls_gen/peer.key $h:/tmp/ic_proxy_tls_test/ || exit 1; done;

--
-- Encryption without certificates: every proxy uses an ephemeral one.
--

-- Switch TLS on (PGC_POSTMASTER: needs a restart).
-- start_ignore
!\retcode gpconfig -c gp_interconnect_proxy_tls_enable -v on;
!\retcode gpstop -ari;
-- end_ignore

-- Motion through the TLS mesh.
2: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;

-- The proxy port speaks TLS 1.3 with the pinned cipher suite and our ALPN.
!\retcode openssl s_client -connect $(psql postgres -Atc "show gp_interconnect_proxy_addresses" | cut -d, -f1 | cut -d: -f3,4) -tls1_3 -alpn ic-proxy/1 </dev/null 2>&1 | grep -q "Cipher is TLS_AES_256_GCM_SHA384";

-- Peer teardown under TLS: kill one segment's proxy.  The postmaster
-- restarts it, the other proxies drop the peer, and the next query goes
-- through again.
!\retcode pkill -f "postgres: *$(psql postgres -Atc "select port from gp_segment_configuration where content=1 and role='p'"), ic proxy process";
!\retcode sleep 3;
2: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;

-- The killed proxy's connections ended with a bare TCP close; the remote
-- proxies must read that as EOF, not as a TLS failure.  Nothing so far
-- gave the proxies a reason for a WARNING.
2: SELECT count(*) AS tls_warnings
   FROM gp_toolkit.gp_log_system
   WHERE logtime > (SELECT max(logtime) FROM gp_toolkit.__gp_log_coordinator_ext
                    WHERE logmessage = 'ic_proxy_tls: test start')
   AND logseverity = 'WARNING'
   AND logmessage LIKE 'ic-proxy TLS:%';

-- A TLS client that is not an ic-proxy peer is turned away.  One that
-- offers no ALPN at all completes the handshake on its side, so its
-- rejection only shows in the proxy's log; one that offers another
-- protocol gets an alert.
!\retcode openssl s_client -connect $(psql postgres -Atc "show gp_interconnect_proxy_addresses" | cut -d, -f1 | cut -d: -f3,4) -tls1_3 </dev/null >/dev/null 2>&1 || true;
!\retcode sleep 1;
2: SELECT count(*) AS no_alpn_refused
   FROM gp_toolkit.__gp_log_coordinator_ext
   WHERE logtime > (SELECT max(logtime) FROM gp_toolkit.__gp_log_coordinator_ext
                    WHERE logmessage = 'ic_proxy_tls: test start')
   AND logseverity = 'WARNING'
   AND logmessage LIKE 'ic-proxy TLS:%the remote did not negotiate ALPN%';
!\retcode openssl s_client -connect $(psql postgres -Atc "show gp_interconnect_proxy_addresses" | cut -d, -f1 | cut -d: -f3,4) -tls1_3 -alpn h2 </dev/null 2>&1 | grep -q "SSL alert number 120";

--
-- Mutual verification.  The certificate, key and CA settings are read on
-- reload, no restart.
--

-- start_ignore
!\retcode gpconfig -c gp_interconnect_proxy_tls_cert_file -v /tmp/ic_proxy_tls_test/peer.crt;
!\retcode gpconfig -c gp_interconnect_proxy_tls_key_file -v /tmp/ic_proxy_tls_test/peer.key;
!\retcode gpconfig -c gp_interconnect_proxy_tls_ca_file -v /tmp/ic_proxy_tls_test/ca.crt;
!\retcode gpstop -u;
-- end_ignore
!\retcode sleep 2;

-- The proxy now presents the certificate of the CA, and wants one too.
!\retcode openssl s_client -connect $(psql postgres -Atc "show gp_interconnect_proxy_addresses" | cut -d, -f1 | cut -d: -f3,4) -tls1_3 -alpn ic-proxy/1 -CAfile /tmp/ic_proxy_tls_gen/ca.crt </dev/null 2>&1 | grep -q "Verify return code: 0 (ok)";

-- A client with a certificate of the CA is let in: no alert for it (grep
-- finds nothing and exits with 1).
!\retcode (sleep 1) | openssl s_client -connect $(psql postgres -Atc "show gp_interconnect_proxy_addresses" | cut -d, -f1 | cut -d: -f3,4) -tls1_3 -alpn ic-proxy/1 -cert /tmp/ic_proxy_tls_gen/peer.crt -key /tmp/ic_proxy_tls_gen/peer.key 2>&1 | grep -q "alert";

-- One with a certificate the CA did not issue is refused, and so is one
-- without a certificate.
!\retcode (sleep 1) | openssl s_client -connect $(psql postgres -Atc "show gp_interconnect_proxy_addresses" | cut -d, -f1 | cut -d: -f3,4) -tls1_3 -alpn ic-proxy/1 -cert /tmp/ic_proxy_tls_gen/rogue.crt -key /tmp/ic_proxy_tls_gen/rogue.key 2>&1 | grep -q "alert unknown ca";
!\retcode (sleep 1) | openssl s_client -connect $(psql postgres -Atc "show gp_interconnect_proxy_addresses" | cut -d, -f1 | cut -d: -f3,4) -tls1_3 -alpn ic-proxy/1 2>&1 | grep -q "alert certificate required";

-- The connections made before the reload are not touched by it.  Make a
-- proxy connect again, so that real peers verify each other, and send
-- data both through those new connections and through the old ones.
!\retcode pkill -f "postgres: *$(psql postgres -Atc "select port from gp_segment_configuration where content=1 and role='p'"), ic proxy process";
!\retcode sleep 3;
2: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;

-- The refused strangers are reported, but they must not have disturbed
-- the healthy connections: no read or write failure on any of them.
2: SELECT count(*) AS broken_connections
   FROM gp_toolkit.gp_log_system
   WHERE logtime > (SELECT max(logtime) FROM gp_toolkit.__gp_log_coordinator_ext
                    WHERE logmessage = 'ic_proxy_tls: test start')
   AND (logmessage LIKE 'ic-proxy TLS:%SSL_read failed%' OR
        logmessage LIKE 'ic-proxy TLS:%SSL_write failed%');

--
-- Settings that cannot be used.
--

-- On reload they are reported and the proxies keep what they have: the
-- same processes go on serving.
-- start_ignore
!\retcode gpconfig -c gp_interconnect_proxy_tls_cert_file -v /nonexistent/peer.crt;
!\retcode gpstop -u;
-- end_ignore
!\retcode sleep 2;
2: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;
2q:

-- At startup there is nothing to keep.  The proxy does not serve, and a
-- proxy-mode query fails at interconnect setup instead of hanging.
-- start_ignore
!\retcode gpstop -ari;
-- end_ignore
3: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;

-- The proxy process waits for better settings, it is not restarted in a
-- loop: the same pid two seconds later.
!\retcode a=$(pgrep -f "postgres: *$(psql postgres -Atc "select port from gp_segment_configuration where content=-1 and role='p'"), ic proxy process"); sleep 2; b=$(pgrep -f "postgres: *$(psql postgres -Atc "select port from gp_segment_configuration where content=-1 and role='p'"), ic proxy process"); test -n "$a" -a "$a" = "$b";

-- A reload with good settings is enough to make it serve.
-- start_ignore
!\retcode gpconfig -c gp_interconnect_proxy_tls_cert_file -v /tmp/ic_proxy_tls_test/peer.crt;
!\retcode gpstop -u;
-- end_ignore
!\retcode sleep 3;
3: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;
3q:

-- Back to plain TCP peers.
-- start_ignore
!\retcode gpconfig -r gp_interconnect_proxy_tls_cert_file;
!\retcode gpconfig -r gp_interconnect_proxy_tls_key_file;
!\retcode gpconfig -r gp_interconnect_proxy_tls_ca_file;
!\retcode gpconfig -c gp_interconnect_proxy_tls_enable -v off;
!\retcode gpstop -ari;
-- end_ignore
4: SELECT count(*) FROM ic_proxy_tls_t a JOIN ic_proxy_tls_t b ON a.i = b.i + 1;

4: DROP TABLE ic_proxy_tls_t;
4q:

!\retcode for h in $(psql postgres -Atc "select distinct hostname from gp_segment_configuration"); do ssh -n $h "rm -rf /tmp/ic_proxy_tls_test" || exit 1; done; rm -rf /tmp/ic_proxy_tls_gen;
