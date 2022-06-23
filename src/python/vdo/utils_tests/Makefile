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
PYLINT_ARGS += --init-hook='import sys; sys.path.append("..")'
endif

UNIT_TEST_FILES = $(wildcard *Test.py)

PYTHON_MODULES := *.py

.PHONY: all dist doc

.PHONY: clean
clean:
	-rm -f *.pyc

UNIT_TEST_MODULES := $(UNIT_TEST_FILES:.py=)

# We do this thing where we run each unit test module individually because
# developers still run make checkin in squeeze, despite automated testing
# not doing so, and squeeze runs python 2.6. The unittest's 'discover'
# functionality was introduced in 2.7. So, while it would be more efficient to
# run all of them at once with the 'discover' feature, we can't yet.
.PHONY: $(UNIT_TEST_MODULES)
$(UNIT_TEST_MODULES):
	PYTHONDONTWRITEBYTECODE=true python $@.py

.PHONY: checkin
checkin: $(UNIT_TEST_MODULES)
