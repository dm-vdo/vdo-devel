# VDO C++ top level Makefile
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$
#

SRC_DIR = ..
include defines

SUBDIRS = tools third uds vdo devices

.PHONY: all
all:
	set -e;				\
	for d in $(SUBDIRS); do		\
		$(MAKE) -C $$d $@;	\
	done

.PHONY: doc
doc:
	set -e;				\
	for d in $(SUBDIRS); do		\
		$(MAKE) -C $$d $@;	\
	done

.PHONY: uds-checkin
uds-checkin: all
	$(MAKE) -C uds checkin

.PHONY: vdo-checkin
vdo-checkin: all
	$(MAKE) -C vdo checkin

.PHONY: checkin
checkin: uds-checkin vdo-checkin

.PHONY: cleanall
cleanall: clean

.PHONY: clean
clean:
	for d in $(SUBDIRS); do 	\
		$(MAKE) -C $$d $@;	\
	done
	rm -rf doxygen
