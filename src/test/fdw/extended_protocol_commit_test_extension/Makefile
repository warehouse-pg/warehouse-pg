MODULES = extended_protocol_commit_test_fdw

EXTENSION = extended_protocol_commit_test_fdw
DATA = extended_protocol_commit_test_fdw--1.0.sql
PGFILEDESC = "extended_protocol_commit_test_fdw - a dummy extension to test extended protocol commit"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/fdw/extended_protocol_commit_test_extension
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
