#include "postgres.h"

#include "access/xlogutils.h"
#include "catalog/pg_database.h"
#include "catalog/storage_database.h"
#include "common/relpath.h"
#include "replication/slot.h"
#include "utils/faultinjector.h"
#include "storage/lmgr.h"
#include "storage/md.h"
#include "storage/standby.h"

typedef struct PendingDbDelete
{
	DbDirNode	dbDirNode;		/* database dir that needs to be deleted */
	bool		atCommit;		/* T=delete at commit; F=delete at abort */
	struct PendingDbDelete *next;		/* linked-list link */
} PendingDbDelete;

static void dropDatabaseDirectory(DbDirNode *deldb, bool isRedo);
static bool firstDirOfDatabase(DbDirNode *deldbs, int i);

static PendingDbDelete *pendingDbDeletes = NULL; /* head of linked list */
static Oid sessionLockMoveDbOid = InvalidOid;

/*
 * ScheduleDbDirDelete
 * 		Schedule dboid dir removal at transaction commit/abort
 */

void
ScheduleDbDirDelete(Oid db_id, Oid tablespace_oid, bool forCommit)
{
	PendingDbDelete *pending;

	/* Add the relation to the list of stuff to delete at commit */
	pending = (PendingDbDelete *)
		MemoryContextAlloc(TopMemoryContext, sizeof(PendingDbDelete));
	pending->atCommit = forCommit;
	pending->dbDirNode = (DbDirNode)
		{.tablespace = tablespace_oid, .database = db_id};
	pending->next = pendingDbDeletes;
	pendingDbDeletes = pending;
}

void
DoPendingDbDeletes(bool isCommit)
{
	PendingDbDelete *pending;
	PendingDbDelete *next;

	for (pending = pendingDbDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		/* unlink list entry first, so we don't retry on failure */
		pendingDbDeletes = next;
		/* do deletion if called for */
		if (pending->atCommit == isCommit)
			dropDatabaseDirectory(&pending->dbDirNode,
								  false);

		pfree(pending);
	}
}

void
MoveDbSessionLockAcquire(Oid db_id)
{
	Assert(sessionLockMoveDbOid == InvalidOid);
	LockSharedObjectForSession(DatabaseRelationId, db_id, 0,
							   AccessExclusiveLock);
	sessionLockMoveDbOid = db_id;
}

void
MoveDbSessionLockRelease()
{
	if (sessionLockMoveDbOid == InvalidOid)
		return;
	
	UnlockSharedObjectForSession(DatabaseRelationId, sessionLockMoveDbOid, 0,
							 AccessExclusiveLock);
	DatabaseStorageResetSessionLock();
}

void
DatabaseStorageResetSessionLock()
{
	sessionLockMoveDbOid = InvalidOid;
}

int
GetPendingDbDeletes(bool forCommit, DbDirNode **ptr)
{
	int			ndbs;
	DbDirNode	*dbptr;
	PendingDbDelete *pending;

	ndbs = 0;
	for (pending = pendingDbDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->atCommit == forCommit)
			ndbs++;
	}
	if (ndbs == 0)
	{
		*ptr = NULL;
		return 0;
	}
	dbptr = (DbDirNode *) palloc(ndbs * sizeof(DbDirNode));
	*ptr = dbptr;
	for (pending = pendingDbDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->atCommit == forCommit)
		{
			*dbptr = pending->dbDirNode;
			dbptr++;
		}
	}
	return ndbs;
}

/*
 * Remove the database directories a transaction scheduled for deletion
 * (ALTER DATABASE SET TABLESPACE, DROP DATABASE). Called once the
 * transaction's outcome is durable: at COMMIT/ROLLBACK PREPARED, and in
 * redo of the commit or abort record that carries the list.
 *
 * At a commit, a directory going away means the database is being dropped or
 * has moved to another tablespace, so in redo this also does what
 * dbase_redo() does for XLOG_DBASE_DROP (DROP DATABASE no longer writes that
 * record; it removes its directories from here): sessions a hot standby has
 * connected to the database are terminated and the database's replication
 * slots are dropped, once per database rather than per directory. An abort
 * leaves the database where it was (a CREATE DATABASE's new directory, a SET
 * TABLESPACE's destination), so none of that applies.
 */
void
DropDatabaseDirectories(DbDirNode *deldbs, int ndeldbs, bool isRedo,
						bool forCommit)
{
	bool		resolveConflicts = isRedo && forCommit;
	int			i;

	if (resolveConflicts)
	{
		for (i = 0; i < ndeldbs; i++)
		{
			if (!firstDirOfDatabase(deldbs, i))
				continue;

			if (InHotStandby)
			{
				/*
				 * Lock database while we resolve conflicts to ensure that
				 * InitPostgres() cannot fully re-execute concurrently. This
				 * avoids backends re-connecting automatically to same
				 * database, which can happen in some cases.
				 *
				 * This will lock out walsenders trying to connect to
				 * db-specific slots for logical decoding too, so it's safe
				 * for us to drop slots.
				 */
				LockSharedObjectForSession(DatabaseRelationId,
										   deldbs[i].database, 0,
										   AccessExclusiveLock);
				ResolveRecoveryConflictWithDatabase(deldbs[i].database);
			}

			/* Drop any database-specific replication slots */
			ReplicationSlotsDropDBSlots(deldbs[i].database);
		}
	}

	for (i = 0; i < ndeldbs; i++)
		dropDatabaseDirectory(&deldbs[i], isRedo);

	if (resolveConflicts && InHotStandby)
	{
		/*
		 * Release the session locks. The transaction has already been marked
		 * committed in redo, so a backend that reconnects now sees the
		 * database's final catalog state: gone, or living in another
		 * tablespace.
		 */
		for (i = 0; i < ndeldbs; i++)
		{
			if (firstDirOfDatabase(deldbs, i))
				UnlockSharedObjectForSession(DatabaseRelationId,
											 deldbs[i].database, 0,
											 AccessExclusiveLock);
		}
	}

#ifdef FAULT_INJECTOR
	if(ndeldbs > 0)
		SIMPLE_FAULT_INJECTOR("after_drop_database_directories");
#endif
}

/*
 *	PostPrepare_DatabaseStorage -- Clean up after a successful PREPARE
 *
 * What we have to do here is throw away the in-memory state about pending
 * db deletes. It's all been recorded in the 2PC state file and
 * we no longer need to track it in memory.
 */
void
PostPrepare_DatabaseStorage()
{
	PendingDbDelete *pendingDbDelete;
	PendingDbDelete *nextDbDelete;

	for (pendingDbDelete = pendingDbDeletes; pendingDbDelete != NULL; pendingDbDelete = nextDbDelete)
	{
		nextDbDelete = pendingDbDelete->next;
		pendingDbDeletes = nextDbDelete;
		/* must explicitly free the list entry */
		pfree(pendingDbDelete);
	}

	DatabaseStorageResetSessionLock();
}

/*
 * This function is similar to dbase_redo() for XLOG_DBASE_DROP
 */
static void
dropDatabaseDirectory(DbDirNode *deldb, bool isRedo)
{
	char *dbpath = GetDatabasePath(deldb->database, deldb->tablespace);

	if (isRedo)
	{
		/* Drop pages for this database that are in the shared buffer cache */
		DropDatabaseBuffers(deldb->database);

		/* Also, clean out any fsync requests that might be pending in md.c */
		ForgetDatabaseSyncRequests(deldb->database);

		/* Clean out the xlog relcache too */
		XLogDropDatabase(deldb->database);
	}

	/*
	 * Remove files from the old tablespace
	 */
	if (!rmtree(dbpath, true))
		ereport(WARNING,
				(errmsg("some useless files may be left behind in old database directory \"%s\"",
						dbpath)));

	pfree(dbpath);
}

/*
 * Is entry i the first in the list for its database? The list may hold one
 * directory per tablespace of the same database.
 */
static bool
firstDirOfDatabase(DbDirNode *deldbs, int i)
{
	int			j;

	for (j = 0; j < i; j++)
	{
		if (deldbs[j].database == deldbs[i].database)
			return false;
	}
	return true;
}
