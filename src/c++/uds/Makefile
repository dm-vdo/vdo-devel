# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#

TOP_DIR = .
include defines

SUBDIRS := kernelLinux userLinux

NO_TAG_SUBDIRS = \
	kernelLinux/build/uds-$(VDO_MARKETING_VERSION) \
	kernelLinux/tests/zubenelgenubi-1

.PHONY: all clean
all clean: src-perl
	set -e; for d in $(SUBDIRS); do $(MAKE) -C $$d $@; done

.PHONY: checkin
checkin: all src-perl
	$(RUN_UDSTESTS) checkin;

.PHONY: doc
doc:

.PHONY: cleanall
cleanall: clean dist-clean
	rm -f ID TAGS

.PHONY: src-perl
src-perl:
	if test -d $(PERL_DIR); then $(MAKE) -C $(PERL_DIR) all; fi

.PHONY: tags FORCE
tags: TAGS

MKID_PRUNE_OPTIONS = $(foreach D,$(NO_TAG_SUBDIRS),--prune=$D)
ID: FORCE
	mkid $(MKID_PRUNE_OPTIONS) . /permabit/build/perl/lastrun/lib

TAGS: ID
	fnid | etags -

###############################################################################
# Distribution builds

.PHONY: dist
dist:
	mkdir -p $(ARCHIVE_DIR)
	set -e; for d in $(SUBDIRS); do $(MAKE) -C $$d $@; done

.PHONY: dist-clean
dist-clean:
	rm -rf $(ARCHIVE_DIR)
	set -e; for d in $(SUBDIRS); do $(MAKE) -C $$d $@; done
