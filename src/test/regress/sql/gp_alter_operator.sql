--
-- Verify that ALTER OPERATOR ... SET (RESTRICT/JOIN) updates pg_operator on
-- the coordinator and on every segment.  Selectivity estimators are looked
-- up wherever planning runs (QE-local planning happens for SPI inside
-- EXECUTE ON ALL SEGMENTS functions and utility-mode connections), so the
-- change must reach all of them.
--
CREATE SCHEMA alterop;
SET search_path = alterop;

CREATE FUNCTION alterop_fn(boolean, boolean) RETURNS boolean
    AS $$ SELECT $1 = $2 $$ LANGUAGE sql IMMUTABLE;
CREATE OPERATOR @+ (LEFTARG = boolean, RIGHTARG = boolean, PROCEDURE = alterop_fn);

-- Sanity: the probe sees one pg_operator row per node (QD + every primary
-- segment), so the mismatch checks below cannot pass vacuously.
SELECT count(*) = (SELECT count(*) + 1 FROM gp_segment_configuration
                   WHERE role = 'p' AND content >= 0) AS all_nodes
FROM (SELECT oprrest FROM pg_operator
      WHERE oprname = '@+' AND oprleft = 'boolean'::regtype
      UNION ALL
      SELECT oprrest FROM gp_dist_random('pg_operator')
      WHERE oprname = '@+' AND oprleft = 'boolean'::regtype) q;

ALTER OPERATOR @+ (boolean, boolean) SET (RESTRICT = eqsel, JOIN = eqjoinsel);

-- The QD row must show the new estimators, and every segment must agree
-- with the QD.  Without the dispatch this returns 'mismatch on seg N' rows.
SELECT oprrest, oprjoin FROM pg_operator
WHERE oprname = '@+' AND oprleft = 'boolean'::regtype;
SELECT 'mismatch on seg ' || seg.gp_segment_id AS problem
FROM pg_operator qd, gp_dist_random('pg_operator') seg
WHERE qd.oprname = '@+' AND seg.oprname = '@+'
  AND qd.oprleft = 'boolean'::regtype AND seg.oprleft = 'boolean'::regtype
  AND (qd.oprrest IS DISTINCT FROM seg.oprrest OR
       qd.oprjoin IS DISTINCT FROM seg.oprjoin);

-- Removing the estimators must be dispatched too.
ALTER OPERATOR @+ (boolean, boolean) SET (RESTRICT = NONE, JOIN = NONE);

SELECT oprrest, oprjoin FROM pg_operator
WHERE oprname = '@+' AND oprleft = 'boolean'::regtype;
SELECT 'mismatch on seg ' || seg.gp_segment_id AS problem
FROM pg_operator qd, gp_dist_random('pg_operator') seg
WHERE qd.oprname = '@+' AND seg.oprname = '@+'
  AND qd.oprleft = 'boolean'::regtype AND seg.oprleft = 'boolean'::regtype
  AND (qd.oprrest IS DISTINCT FROM seg.oprrest OR
       qd.oprjoin IS DISTINCT FROM seg.oprjoin);

RESET search_path;
DROP SCHEMA alterop CASCADE;
