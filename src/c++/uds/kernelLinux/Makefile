# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#

TOP_DIR = ..
include $(TOP_DIR)/defines

SUBDIRS = build tests

.PHONY: all clean
all clean:
	set -e; for d in $(SUBDIRS); do $(MAKE) -C $$d $@; done

all: SUBDIRS = build tests

.PHONY: dist dist-clean
dist dist-clean:

.PHONY: src-perl
src-perl:
	if test -d $(PERL_DIR); then $(MAKE) -C $(PERL_DIR) all; fi
