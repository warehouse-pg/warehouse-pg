MODULES = no_rescan_test_fdw

EXTENSION = no_rescan_test_fdw
DATA = no_rescan_test_fdw--1.0.sql
PGFILEDESC = "no_rescan_test_fdw - a dummy extension to test FDW handling not rescannable"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/fdw/no_rescan_test_extension
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
