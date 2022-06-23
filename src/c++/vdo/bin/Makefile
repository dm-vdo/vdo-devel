# VDO C bin Makefile
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$
#

SRC_DIR = ../../..
include $(SRC_DIR)/c++/defines
include $(CURRENT_VERSION_FILE)

PROGS      =
SUBDIRS    =

all: $(PROGS)
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

clean:
	rm -f $(PROGS) *.o core*
	rm -fr $(DEPDIR)
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

doc:

.PHONY: all clean doc
