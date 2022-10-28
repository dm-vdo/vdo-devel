# SPDX-License-Identifier: GPL-2.0-only
#
# VDO C top level Makefile
#
# Copyright Red Hat
#

SRC_DIR = ../..
include $(SRC_DIR)/c++/defines

CHECKIN_SUBDIRS = user tests
SUBDIRS = base bin user tools fake tests kernel

include ../../tools/installers/CURRENT_VERSION

.PHONY: all
all:
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

.PHONY: doc
doc:
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

.PHONY: checkin
checkin: all
	for d in $(CHECKIN_SUBDIRS); do		\
		$(MAKE) -C $$d $@;		\
	done

.PHONY: clean
clean:
	for d in $(SUBDIRS); do 		\
		$(MAKE) -C $$d $@;		\
	done
