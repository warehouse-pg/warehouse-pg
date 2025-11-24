subdir=src/backend/cdb/motion
top_builddir=../../../../..
include $(top_builddir)/src/Makefile.global

TARGETS=cdbsendonebyte

include $(top_builddir)/src/backend/mock.mk

cdbsendonebyte.t: EXCL_OBJS += src/backend/cdb/motion/ic_udpifc.o
cdbsendonebyte.t: \
	$(MOCK_DIR)/backend/access/hash/hash_mock.o \
	$(MOCK_DIR)/backend/utils/fmgr/fmgr_mock.o
