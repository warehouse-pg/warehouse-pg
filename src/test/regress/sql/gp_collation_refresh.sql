--
-- Verify that ALTER COLLATION ... REFRESH VERSION updates pg_collation on
-- the coordinator and on every segment.  The collation version mismatch
-- check runs on whichever node actually uses the collation, so a refresh
-- must reach all of them.
--
-- Only ICU collations carry a version, so skip on builds without ICU (no
-- ICU collations imported by initdb) or databases where ICU collations
-- cannot be created.
SELECT getdatabaseencoding() <> 'UTF8'
       OR NOT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'en-x-icu')
       AS skip_test \gset
\if :skip_test
\quit
\endif

-- Suppress messages that embed the environment's ICU version string.
SET client_min_messages = error;

-- CREATE COLLATION is dispatched, so the stale version lands on all nodes.
CREATE COLLATION coll_refresh_c (provider = icu, locale = 'en-US', version = '0.0');

SELECT count(*) = (SELECT count(*) + 1 FROM gp_segment_configuration
                   WHERE role = 'p' AND content >= 0) AS all_nodes,
       count(DISTINCT collversion) = 1 AS consistent,
       min(collversion) = '0.0'        AS stale
FROM (SELECT collversion FROM pg_collation WHERE collname = 'coll_refresh_c'
      UNION ALL
      SELECT collversion FROM gp_dist_random('pg_collation')
      WHERE collname = 'coll_refresh_c') q;

ALTER COLLATION coll_refresh_c REFRESH VERSION;

-- Every node must have re-derived its actual ICU version.
SELECT count(*) = (SELECT count(*) + 1 FROM gp_segment_configuration
                   WHERE role = 'p' AND content >= 0) AS all_nodes,
       count(DISTINCT collversion) = 1 AS consistent,
       bool_and(collversion <> '0.0')  AS refreshed
FROM (SELECT collversion FROM pg_collation WHERE collname = 'coll_refresh_c'
      UNION ALL
      SELECT collversion FROM gp_dist_random('pg_collation')
      WHERE collname = 'coll_refresh_c') q;

DROP COLLATION coll_refresh_c;
RESET client_min_messages;
