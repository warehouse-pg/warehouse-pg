-- CVE-2026-14666: role membership and role attribute changes must
-- invalidate role-dependent cached plans (row-level security).
--
-- The upstream fix (f4174aa84) registers syscache callbacks on pg_auth_members,
-- pg_authid and pg_database in plancache.c.  This test covers the two places
-- where an MPP cluster can hold such a plan:
--
--   (1) the coordinator plan cache: a PREPAREd statement whose rewrite picked
--       the RLS policies for the role.  A role change committed by *another*
--       session must cause the next EXECUTE to re-rewrite, re-plan and
--       re-dispatch the plan.  QEs never cache a dispatched PlannedStmt (see
--       exec_mpp_query), so once the coordinator replans, the segments execute
--       the new plan.
--
--   (2) a QE-local plan cache: PL/pgSQL functions executed on the segments
--       keep their SPI plans in the QE backend's plancache.  A QE may only
--       read catalog or replicated tables from such a function, and replicated
--       tables can have RLS, so a stale per-QE plan would be reachable there.
--       The role DDL is dispatched to the segments under two-phase commit, so
--       each segment emits its own sinval events and the same callbacks fire in
--       the QE backends at the start of their next transaction.
--
-- The utility-mode (0U) queries confirm the segment catalog actually changed,
-- i.e. that the invalidation originates on the segment itself.

0: CREATE ROLE rls_pc_grp NOLOGIN;
0: CREATE ROLE rls_pc_bob LOGIN NOBYPASSRLS;

-- (1) distributed table, policy only for members of rls_pc_grp
0: CREATE TABLE rls_pc_t (a int) DISTRIBUTED BY (a);
0: INSERT INTO rls_pc_t SELECT generate_series(1, 10);
0: GRANT SELECT ON rls_pc_t TO PUBLIC;
0: ALTER TABLE rls_pc_t ENABLE ROW LEVEL SECURITY;
0: CREATE POLICY p_grp ON rls_pc_t TO rls_pc_grp USING (true);

-- (2) replicated table + function that reads it on the segments
0: CREATE TABLE rls_pc_rep (a int) DISTRIBUTED REPLICATED;
0: INSERT INTO rls_pc_rep SELECT generate_series(1, 10);
0: GRANT SELECT ON rls_pc_rep TO PUBLIC;
0: ALTER TABLE rls_pc_rep ENABLE ROW LEVEL SECURITY;
0: CREATE POLICY p_grp ON rls_pc_rep TO rls_pc_grp USING (true);
0: CREATE FUNCTION rls_pc_count_on_seg() RETURNS bigint LANGUAGE plpgsql STABLE AS $$ DECLARE n bigint; BEGIN SELECT count(*) INTO n FROM rls_pc_rep; RETURN n; END $$;

-- Session 1 acts as rls_pc_bob, who is not (yet) a member of rls_pc_grp.
1: SET ROLE rls_pc_bob;
1: PREPARE q AS SELECT count(*) FROM rls_pc_t;
1: EXECUTE q;
1: SELECT rls_pc_count_on_seg() FROM gp_dist_random('gp_id');

-- Membership granted by another session: both cached plans must be dropped.
0: GRANT rls_pc_grp TO rls_pc_bob;
0U: SELECT count(*) FROM pg_auth_members m JOIN pg_authid g ON g.oid = m.roleid JOIN pg_authid u ON u.oid = m.member WHERE g.rolname = 'rls_pc_grp' AND u.rolname = 'rls_pc_bob';
1: EXECUTE q;
1: SELECT rls_pc_count_on_seg() FROM gp_dist_random('gp_id');

-- Membership revoked: plans must be dropped again.
0: REVOKE rls_pc_grp FROM rls_pc_bob;
0U: SELECT count(*) FROM pg_auth_members m JOIN pg_authid g ON g.oid = m.roleid JOIN pg_authid u ON u.oid = m.member WHERE g.rolname = 'rls_pc_grp' AND u.rolname = 'rls_pc_bob';
1: EXECUTE q;
1: SELECT rls_pc_count_on_seg() FROM gp_dist_random('gp_id');

-- Role attribute change (pg_authid): BYPASSRLS on, then off.
0: ALTER ROLE rls_pc_bob BYPASSRLS;
0U: SELECT rolbypassrls FROM pg_authid WHERE rolname = 'rls_pc_bob';
1: EXECUTE q;
1: SELECT rls_pc_count_on_seg() FROM gp_dist_random('gp_id');

0: ALTER ROLE rls_pc_bob NOBYPASSRLS;
0U: SELECT rolbypassrls FROM pg_authid WHERE rolname = 'rls_pc_bob';
1: EXECUTE q;
1: SELECT rls_pc_count_on_seg() FROM gp_dist_random('gp_id');

1: DEALLOCATE q;
1: RESET ROLE;
1q:

0: DROP FUNCTION rls_pc_count_on_seg();
0: DROP TABLE rls_pc_rep;
0: DROP TABLE rls_pc_t;
0: DROP ROLE rls_pc_bob;
0: DROP ROLE rls_pc_grp;
