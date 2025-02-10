#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/scripts/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "PostgreSQL utility"
PGAPPICON=win32

subdir = src/bin/scripts
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# The frontend doesn't need everything that's in LIBS, some are backend only
LIBS := $(filter-out -lresolv -lbz2, $(LIBS))
# This program isn't interactive, so doesn't need these
LIBS := $(filter-out -lreadline -ledit -ltermcap -lncurses -lcurses -lcurl -lssl -lcrypto -lz, $(LIBS))


PROGRAMS = createdb createlang createuser dropdb droplang dropuser clusterdb vacuumdb reindexdb pg_isready

override CPPFLAGS := -DFRONTEND -I$(top_srcdir)/src/bin/pg_dump -I$(top_srcdir)/src/bin/psql -I$(libpq_srcdir) $(CPPFLAGS)


all: $(PROGRAMS)

ifeq ($(PORTNAME),openbsd)
override CFLAGS += -pthread
endif

%: %.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

createdb: createdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
createlang: createlang.o common.o dumputils.o kwlookup.o keywords.o print.o mbprint.o | submake-libpq submake-libpgport
createuser: createuser.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
dropdb: dropdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
droplang: droplang.o common.o dumputils.o kwlookup.o keywords.o print.o mbprint.o | submake-libpq submake-libpgport
dropuser: dropuser.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
clusterdb: clusterdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
vacuumdb: vacuumdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
reindexdb: reindexdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
pg_isready: pg_isready.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport

$(PROGRAMS): encnames.o wchar.o

# We use a few backend modules verbatim, but since we need
# to compile with appropriate options to build a shared lib, we can't
# use the same object files built for the backend.
# Instead, symlink the source files in here and build our own object files.
# When you add a file here, remember to add it in the "clean" target below.
encnames.c wchar.c: % : $(top_srcdir)/src/backend/utils/mb/%
	rm -f $@ && $(LN_S) $< .

dumputils.c keywords.c: % : $(top_srcdir)/src/bin/pg_dump/%
	rm -f $@ && $(LN_S) $< .

print.c mbprint.c : % : $(top_srcdir)/src/bin/psql/%
	rm -f $@ && $(LN_S) $< .

kwlookup.c: % : $(top_srcdir)/src/backend/parser/%
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) createdb$(X)   '$(DESTDIR)$(bindir)'/createdb$(X)
	$(INSTALL_PROGRAM) dropdb$(X)     '$(DESTDIR)$(bindir)'/dropdb$(X)
	$(INSTALL_PROGRAM) createlang$(X) '$(DESTDIR)$(bindir)'/createlang$(X)
	$(INSTALL_PROGRAM) droplang$(X)   '$(DESTDIR)$(bindir)'/droplang$(X)
	$(INSTALL_PROGRAM) createuser$(X) '$(DESTDIR)$(bindir)'/createuser$(X)
	$(INSTALL_PROGRAM) dropuser$(X)   '$(DESTDIR)$(bindir)'/dropuser$(X)
	$(INSTALL_PROGRAM) clusterdb$(X)  '$(DESTDIR)$(bindir)'/clusterdb$(X)
	$(INSTALL_PROGRAM) vacuumdb$(X)   '$(DESTDIR)$(bindir)'/vacuumdb$(X)
	$(INSTALL_PROGRAM) reindexdb$(X)  '$(DESTDIR)$(bindir)'/reindexdb$(X)
	$(INSTALL_PROGRAM) pg_isready$(X) '$(DESTDIR)$(bindir)'/pg_isready$(X)

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, $(addsuffix $(X), $(PROGRAMS)))

clean distclean maintainer-clean:
	rm -f $(addsuffix $(X), $(PROGRAMS)) $(addsuffix .o, $(PROGRAMS))
	rm -f common.o dumputils.o kwlookup.o keywords.o print.o mbprint.o $(WIN32RES)
	rm -f encnames.o wchar.o
	rm -f dumputils.c print.c mbprint.c kwlookup.c keywords.c
	rm -rf tmp_check

check: all
	$(prove_check)

installcheck:
	$(prove_installcheck)
