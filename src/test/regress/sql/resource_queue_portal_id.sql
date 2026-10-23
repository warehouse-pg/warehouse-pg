--
-- Resource queue portal ids.
--
-- A portal gets its resource queue portal id when it is created, but takes its
-- queue slot only when it starts.  A portal created by EXECUTE (or by Bind for
-- a named portal), and a cursor opened through SPI, exist before their query
-- is planned, and the planner evaluates a stable function with constant
-- arguments.  If that function opens a cursor, the cursor must not be given
-- the same portal id: the outer portal then failed with "duplicate portal id",
-- and the cursor's queue slot was never released.
--
CREATE RESOURCE QUEUE rq_portal_id WITH (ACTIVE_STATEMENTS = 10);
CREATE ROLE rq_portal_id_role RESOURCE QUEUE rq_portal_id;

CREATE FUNCTION rq_portal_id_open(p_cur OUT refcursor, p_limit int) AS $$
BEGIN
	OPEN p_cur FOR SELECT g FROM generate_series(1, p_limit) g;
END;
$$ LANGUAGE plpgsql STABLE;

CREATE FUNCTION rq_portal_id_open_volatile(p_cur OUT refcursor, p_limit int) AS $$
BEGIN
	OPEN p_cur FOR SELECT g FROM generate_series(1, p_limit) g;
END;
$$ LANGUAGE plpgsql VOLATILE;

SET ROLE rq_portal_id_role;

-- EXECUTE creates its portal before planning.
PREPARE rq_portal_id_stable(int) AS
	SELECT p_cur IS NOT NULL AS opened FROM rq_portal_id_open($1) AS f(p_cur);
EXECUTE rq_portal_id_stable(0);
EXECUTE rq_portal_id_stable(3);

-- A cursor opened through SPI under a portal that holds no slot (DO).
DO $$
DECLARE
	c refcursor;
BEGIN
	OPEN c FOR SELECT * FROM rq_portal_id_open(0);
	CLOSE c;
END
$$;

-- Control: a volatile function is not evaluated at plan time.
PREPARE rq_portal_id_volatile(int) AS
	SELECT p_cur IS NOT NULL AS opened FROM rq_portal_id_open_volatile($1) AS f(p_cur);
EXECUTE rq_portal_id_volatile(0);

RESET ROLE;

-- No statement may still hold a slot in the queue.
SELECT rsqcountvalue, rsqholders FROM gp_toolkit.gp_resqueue_status
	WHERE rsqname = 'rq_portal_id';

DEALLOCATE rq_portal_id_stable;
DEALLOCATE rq_portal_id_volatile;
DROP FUNCTION rq_portal_id_open(int);
DROP FUNCTION rq_portal_id_open_volatile(int);
DROP ROLE rq_portal_id_role;
DROP RESOURCE QUEUE rq_portal_id;
