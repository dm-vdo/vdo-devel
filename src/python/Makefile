#
# %COPYRIGHT%
#
# %LICENSE%
#
# Python top level Makefile
#
# $Id$
#

SUBDIRS = bin

.PHONY: all check checkin clean dist doc
all check checkin clean dist doc:
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

.PHONY: cleanall
cleanall: clean
