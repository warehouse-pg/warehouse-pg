/* gpcontrib/gp_toolkit/gp_toolkit--1.7--1.8.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION gp_toolkit UPDATE TO '1.8'" to load this file. \quit

CREATE OR REPLACE VIEW gp_toolkit.gp_resgroup_config AS
SELECT
    g.oid AS groupid,
    g.rsgname AS groupname,
    MAX(c.value) FILTER (WHERE c.reslimittype = 1) AS concurrency,
    MAX(c.value) FILTER (WHERE c.reslimittype = 2) AS cpu_max_percent,
    MAX(c.value) FILTER (WHERE c.reslimittype = 3) AS cpu_weight,
    MAX(c.value) FILTER (WHERE c.reslimittype = 4) AS cpuset,
    MAX(c.value) FILTER (WHERE c.reslimittype = 5) AS memory_quota,
    MAX(c.value) FILTER (WHERE c.reslimittype = 6) AS min_cost,
    MAX(c.value) FILTER (WHERE c.reslimittype = 7) AS io_limit
FROM pg_resgroup g
         LEFT JOIN pg_resgroupcapability c ON g.oid = c.resgroupid
GROUP BY g.oid, g.rsgname;

GRANT SELECT ON gp_toolkit.gp_resgroup_config TO public;
