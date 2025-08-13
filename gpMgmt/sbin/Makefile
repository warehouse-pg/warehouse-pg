# gpMgmt/sbin/Makefile

top_builddir = ../..
ifneq "$(wildcard $(top_builddir)/src/Makefile.global)" ""
include $(top_builddir)/src/Makefile.global
endif

PYCS= gpconfig_helper.pyc gpoperation.pyc gpsegstop.pyc gpcleansegmentdir.pyc \
	gpgetstatususingtransition.pyc gpsegstart.pyc seg_update_pg_hba.pyc \
	gpsegrecovery.pyc gpsegsetuprecovery.pyc recovery_base.pyc

PY_PROGRAMS= gpconfig_helper.py gpoperation.py gpsegstop.py gpcleansegmentdir.py \
	gpgetstatususingtransition.py gpsegstart.py packcore seg_update_pg_hba.py \
	gpsegrecovery.py gpsegsetuprecovery.py recovery_base.py

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)/python'
	$(MKDIR_P) '$(DESTDIR)$(sbindir)'

install: installdirs
	# Put the python executables next to the gppylib, so they can find it.
	for file in $(PY_PROGRAMS); do \
		$(INSTALL_SCRIPT) $$file '$(DESTDIR)$(libdir)/python/'$$file ; \
		$(PERL) $(top_builddir)/putversion '$(DESTDIR)$(libdir)/python/'$$file ; \
		cd $(DESTDIR)$(sbindir)/ && $(LN_S) ../lib/python/$$file . ; \
		cd $(CURDIR) ; \
	done

uninstall:
	for file in $(PY_PROGRAMS); do \
		rm -f '$(DESTDIR)$(sbindir)/'$$file ; \
		rm -f '$(DESTDIR)$(libdir)/python/'$$file ; \
	done

clean distclean:
	rm -f $(PYCS)
