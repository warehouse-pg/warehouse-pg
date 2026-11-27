# src/test/modules/test_orca_regions/Makefile

MODULES = test_orca_regions
PGFILEDESC = "test_orca_regions - test ORCA's extension hooks"

ifdef USE_PGXS
REGRESS = test_orca_regions
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_orca_regions
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
# the hooks are part of ORCA
ifeq ($(enable_orca),yes)
REGRESS = test_orca_regions
endif
include $(top_srcdir)/contrib/contrib-global.mk
endif
