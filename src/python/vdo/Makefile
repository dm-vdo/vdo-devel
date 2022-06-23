#
# %COPYRIGHT%
#
# %LICENSE%
#

#
# VDO Python Makefile
#
# $Id$
#

# When building the dist, we don't include the python tree so only
# include this if someone is trying to run `make check' or 'make pylint'
ifneq (,$(filter check pylint,$(MAKECMDGOALS)))
include ../Makefile.common
endif

PYLINT_ARGS    += --additional-builtins=_
PYTHON_SCRIPTS += dmdevice vdo

DIST_PROGS = 

SUBDIRS = utils

CHECK_SUBDIRS = utils

CHECKIN_SUBDIRS = utils_tests

all:
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

clean: clean_gettext
	set -e;					\
	for d in $(CHECK_SUBDIRS) $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

check: check_gettext

check:
	set -e;					\
	for d in $(CHECK_SUBDIRS); do		\
		$(MAKE) -C $$d $@;		\
	done

check_gettext gettext:
	xgettext -L Python $(PYTHON_MODULES) */*.py

clean_gettext:
	$(RM) messages.po

checkin:
	set -e;					\
	for d in $(CHECKIN_SUBDIRS); do		\
		$(MAKE) -C $$d $@;		\
	done


dist: $(DIST_PROGS)
	if test -z "$(VDO_DIST_DIR)" ; then \
	  echo No distribution directory specified. ; \
	  exit 1 ; \
	else : ; \
	fi
	mkdir -p $(VDO_DIST_DIR)/bin
	cp -pvf $(DIST_PROGS) $(VDO_DIST_DIR)/bin
	set -e;                                  \
	for i in $(SUBDIRS); do                  \
	  $(MAKE) -C $$i dist;                   \
        done

doc:

pylint:

.PHONY: all check checkin check_gettext clean clean_gettext dist doc \
	gettext
