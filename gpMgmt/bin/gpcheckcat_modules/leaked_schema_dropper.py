from gppylib.utils import escapeDoubleQuoteInSQLString

class LeakedSchemaDropper:

    # This query does a union of all the leaked temp schemas on the coordinator as well as all the segments.
    # The first part of the query uses gp_dist_random which gets the leaked schemas from only the segments
    # The second part of the query gets the leaked temp schemas from just the coordinator
    # The simpler form of this query that pushed the union into the
    # inner select does not run correctly on 3.2.x
    # Also note autovacuum lancher and worker will not generate temp namespace
    #
    # On the segments, only temp schemas created in normal mode are named
    # pg_temp_<gp_session_id>; utility mode names them pg_temp_0<backend id>
    # (see InitTempTableNamespace()).  A backend id must not be matched
    # against the coordinator's session ids: such a schema would be kept
    # whenever an unrelated coordinator process happens to hold that session
    # id, and dropped while its utility-mode session is still alive.  Backend
    # ids are not exposed in pg_stat_activity, so the third part treats a
    # utility-mode schema as leaked only when no segment that has it has a
    # live utility-mode connection (a client backend without a session id)
    # to this database.  The DROP is dispatched to every instance, so a name
    # that might be in use on one segment is kept on all of them, and a name
    # that also exists on the coordinator is left to the second part.
    leaked_schema_query = """
        SELECT distinct nspname as schema
        FROM (
          SELECT nspname, replace(nspname, 'pg_temp_','')::int as sess_id
          FROM   gp_dist_random('pg_namespace')
          WHERE  nspname ~ '^pg_temp_[1-9][0-9]*'
          UNION ALL
          SELECT nspname, replace(nspname, 'pg_toast_temp_','')::int as sess_id
          FROM   gp_dist_random('pg_namespace')
          WHERE  nspname ~ '^pg_toast_temp_[1-9][0-9]*'
        ) n LEFT OUTER JOIN pg_stat_activity x using (sess_id)
        WHERE x.sess_id is null OR x.backend_type like 'autovacuum%'
        UNION
        SELECT nspname as schema
        FROM (
          SELECT nspname, replace(nspname, 'pg_temp_','')::int as sess_id
          FROM   pg_namespace
          WHERE  nspname ~ '^pg_temp_[0-9]+'
          UNION ALL
          SELECT nspname, replace(nspname, 'pg_toast_temp_','')::int as sess_id
          FROM   pg_namespace
          WHERE  nspname ~ '^pg_toast_temp_[0-9]+'
        ) n LEFT OUTER JOIN pg_stat_activity x using (sess_id)
        WHERE x.sess_id is null OR x.backend_type like 'autovacuum%'
        UNION
        SELECT nspname as schema
        FROM   gp_dist_random('pg_namespace') n
        LEFT OUTER JOIN (
          SELECT DISTINCT gp_segment_id
          FROM   gp_stat_activity
          WHERE  gp_segment_id >= 0
          AND    backend_type = 'client backend'
          AND    sess_id = -1
          AND    datname = current_database()
        ) x ON x.gp_segment_id = n.gp_segment_id
        WHERE  nspname ~ '^pg_(toast_)?temp_0[0-9]+'
        AND    nspname NOT IN (SELECT nspname FROM pg_namespace)
        GROUP BY nspname
        HAVING count(x.gp_segment_id) = 0
    """

    def __get_leaked_schemas(self, db_connection):
        with db_connection.cursor() as curs:
            curs.execute(self.leaked_schema_query)
            leaked_schemas = curs.fetchall()

            if not leaked_schemas:
                return []

            return [row[0] for row in leaked_schemas if row[0]]

    def drop_leaked_schemas(self, db_connection):
        leaked_schemas = self.__get_leaked_schemas(db_connection)
        for leaked_schema in leaked_schemas:
            escaped_schema_name = escapeDoubleQuoteInSQLString(leaked_schema)
            with db_connection.cursor() as curs:
                curs.execute('DROP SCHEMA IF EXISTS %s CASCADE;' % (escaped_schema_name))
        return leaked_schemas
