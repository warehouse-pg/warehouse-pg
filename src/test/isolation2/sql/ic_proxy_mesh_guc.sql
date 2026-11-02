-- gp_interconnect_proxy_addresses is validated when it is set.
--
-- The grammar is the one the proxy's own parser (a fscanf loop in
-- ic_proxy_addr.c) has always consumed, so values that worked before the
-- check hook existed keep working: blanks around the entries and a trailing
-- comma are accepted.  Within an entry the fields are strict, and a value
-- that the proxy would have silently truncated or mis-parsed is rejected
-- here with a message that says which entry is wrong.
--
-- ALTER SYSTEM only writes postgresql.auto.conf on the coordinator and the
-- accepted value is reset right away, so the running mesh is never touched.
--
-- The hostname limit is the proxy's HOST_NAME_MAX buffer, which differs by
-- platform (64 on Linux, 255 where the macro is missing), so the numbers
-- are masked.
-- start_matchsubs
-- m/\(\d+ bytes, maximum is \d+\)/
-- s/\(\d+ bytes, maximum is \d+\)/(N bytes, maximum is M)/
-- end_matchsubs

ALTER SYSTEM SET gp_interconnect_proxy_addresses = ' 1:-1:cdw:2000, 2:0:sdw1:2000 ,3:1:sdw1:2001,';
ALTER SYSTEM RESET gp_interconnect_proxy_addresses;

-- malformed entry
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-1:cdw:2000,junk';
-- missing field
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-1:cdw';
-- dbid and content ranges
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '0:-1:cdw:2000';
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-2:cdw:2000';
-- port range
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-1:cdw:2000,2:0:sdw1:70000';
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-1:cdw:0';
-- hostname with a blank inside, and one that does not fit the proxy's buffer
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-1:cdw:2000,2:0:sdw 1:2001';
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-1:cdw:2000,2:0:hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh:2001';
-- stray separator
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-1:cdw:2000;2:0:sdw1:2001';
-- a dbid listed twice, and two proxies on one host:port
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-1:cdw:2000,2:0:sdw1:2000,2:1:sdw1:2001';
ALTER SYSTEM SET gp_interconnect_proxy_addresses = '1:-1:cdw:2000,2:0:sdw1:2000,3:1:sdw1:2000';

-- the running value is untouched and still visible (unhidden GUC)
SELECT current_setting('gp_interconnect_proxy_addresses') <> '' AS mesh_visible;
