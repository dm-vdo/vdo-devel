#
# %COPYRIGHT%
#
# %LICENSE%
#

# $Id$

SRC_DIR = ../../..

# When building the dist, we don't include the python tree so only
# include this if someone is trying to run `make check' or 'make pylint'
ifneq (,$(filter check pylint,$(MAKECMDGOALS)))
include ../../Makefile.common
endif

PYTHON_MODULES  = $(wildcard *.py)

all:;

clean:
	-rm -f *.pyc

dist: $(PYTHON_MODULES)
	if test -z "$(VDO_DIST_DIR)" ; then \
	  echo No distribution directory specified. ; \
	  exit 1 ; \
	else : ; \
	fi
	mkdir -p $(VDO_DIST_DIR)/bin/utils
	cp -pvf $(PYTHON_MODULES) $(VDO_DIST_DIR)/bin/utils

doc:;

.PHONY: all clean dist doc
