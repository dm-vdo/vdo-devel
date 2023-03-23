#
# Copyright (c) 2017, 2018 Red Hat, Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it would be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write the Free Software Foundation,
# Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
#

# $Id$

SRC_DIR = ../..
include ../defines

PREPARE_SOURCE_DIST ?= $(SRC_DIR)/perl/bin/prepareSourceDist.pl

RPMBUILD      = rpmbuild
RPMBUILD_FLAG = -bs
BUILD_DIR    ?= $(realpath .)/build
SOURCES       = $(BUILD_DIR)/SOURCES
SPECS         = $(BUILD_DIR)/SPECS

# rpmbuild fails to clean up after itself on RHEL8 if BUILDROOT is under
# BUILD_DIR in NFS so just pick a temporary local directory for it to use.
BUILDROOT := $(shell mktemp -du BUILDROOT-XXXXXXXXXX --tmpdir)

.PHONY: all
all: srpms

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

$(SOURCES): $(VDO_ROOT)
	mkdir -p $(SOURCES)
	$(PREPARE_SOURCE_DIST) --dest=$(SOURCES) \
	    $(CHANGELOG_FLAGS) $(VDO_ROOT) > $(SOURCES)/prepareSourceDist.out

.PHONY: srpm
srpm: $(SOURCES)
	mkdir -p $(SPECS)
	cp $(SOURCES)/$(SPEC) $(SPECS)
	cd $(SPECS) && $(RPMBUILD) $(RPMBUILD_FLAG) \
	  --define '_topdir $(realpath $(BUILD_DIR))' \
	  --buildroot=$(BUILDROOT) $(SPEC)

.PHONY: vdo-srpm
vdo-srpm: SPEC=vdo.spec
vdo-srpm: srpm

.PHONY: kvdo-srpm
kvdo-srpm: SPEC=kvdo.spec
kvdo-srpm: srpm

.PHONY: srpms
srpms:
	$(MAKE) vdo-srpm
	$(MAKE) kvdo-srpm

.PHONY: test-srpms
test-srpms:
	make VDO_ROOT=$(TEST_ROOT) srpms
