SUBDIRS = extended_protocol_commit_test_extension no_rescan_test_extension

PROGRAM = extended_protocol_commit_test

OBJS = extended_protocol_commit_test.o

REGRESS = extended_protocol_commit_test no_rescan_test
REGRESS_OPTS = --init-file=init_file

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/fdw
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
override LDFLAGS := $(libpq_pgport) $(LDFLAGS)

installcheck: install

$(call recurse,all install clean)
