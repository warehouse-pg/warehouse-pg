/*-------------------------------------------------------------------------
 *
 * pg_backup_restrict.c
 *	  Restrict-key helpers for pg_dump, pg_dumpall and pg_restore, used to
 *	  emit psql's \restrict / \unrestrict meta-commands (CVE-2025-8714).
 *
 * These helpers deliberately live in their own translation unit instead of
 * in the shared dumputils.c.  generate_restrict_key() calls
 * pg_strong_random(), which in an --with-openssl build references libcrypto.
 * dumputils.c is also compiled into src/bin/scripts and src/bin/psql, whose
 * programs do not link libcrypto; pulling the OpenSSL dependency into their
 * dumputils.o would break their link ("undefined reference to RAND_status").
 * Keeping this code in an object that only pg_dump/pg_dumpall/pg_restore link
 * confines the dependency to programs that already link libcrypto.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/pg_backup_restrict.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "dumputils.h"

static const char restrict_chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

/*
 * Generates a valid restrict key (i.e., an alphanumeric string) for use with
 * psql's \restrict and \unrestrict meta-commands.  For safety, the value is
 * chosen at random.
 */
char *
generate_restrict_key(void)
{
	uint8		buf[64];
	char	   *ret = pg_malloc(sizeof(buf));

	if (!pg_strong_random(buf, sizeof(buf)))
		return NULL;

	for (int i = 0; i < sizeof(buf) - 1; i++)
	{
		uint8		idx = buf[i] % strlen(restrict_chars);

		ret[i] = restrict_chars[idx];
	}
	ret[sizeof(buf) - 1] = '\0';

	return ret;
}

/*
 * Checks that a given restrict key (intended for use with psql's \restrict and
 * \unrestrict meta-commands) contains only alphanumeric characters.
 */
bool
valid_restrict_key(const char *restrict_key)
{
	return restrict_key != NULL &&
		restrict_key[0] != '\0' &&
		strspn(restrict_key, restrict_chars) == strlen(restrict_key);
}
