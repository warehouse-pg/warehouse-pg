subdir=src/backend/utils/adt
top_builddir=../../../../..
include $(top_builddir)/src/Makefile.global

TARGETS=runtimefilter

include $(top_srcdir)/src/backend/mock.mk

# Everything the filter uses is real (memory contexts, pg_popcount, the hash
# functions). The two mocks only satisfy the linker for objects mock.mk leaves
# out (fmgrtab.o, access/hash/hash.o), as in the other test directories.
runtimefilter.t: \
	$(MOCK_DIR)/backend/access/hash/hash_mock.o \
	$(MOCK_DIR)/backend/utils/fmgr/fmgr_mock.o
