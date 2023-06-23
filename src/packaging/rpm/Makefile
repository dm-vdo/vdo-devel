# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#

SRC_DIR = ../..
include ../defines

PREPARE_SOURCE_DIST ?= $(SRC_DIR)/perl/bin/prepareSourceDist.pl

RPMBUILD      = rpmbuild
RPMBUILD_FLAG = -bs
BUILD_DIR    ?= $(realpath .)/build
WORK_DIR     ?= $(realpath .)/work
PACKAGE       = kvdo-$(VDO_VERSION)
PREPARED_DIR  = $(WORK_DIR)/$(PACKAGE)
SOURCES       = $(BUILD_DIR)/SOURCES

SPECS         = $(BUILD_DIR)/SPECS
MANIFEST     ?= src/packaging/rpm/MANIFEST.yaml

# rpmbuild fails to clean up after itself on RHEL8 if BUILDROOT is under
# BUILD_DIR in NFS so just pick a temporary local directory for it to use.
BUILDROOT := $(shell mktemp -du BUILDROOT-XXXXXXXXXX --tmpdir)

RPMBUILD_ARGS = --define '_topdir $(realpath $(BUILD_DIR))' \
		--buildroot=$(BUILDROOT)

.PHONY: all
all: srpms

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(WORK_DIR) prepare.out

prepare: prepare.out

.PHONY: FORCE
prepare.out: FORCE
	rm -rf $(WORK_DIR)
	mkdir -p $(WORK_DIR)
	$(PREPARE_SOURCE_DIST) --dest=$(WORK_DIR) --manifest=$(MANIFEST) \
	    $(CHANGELOG_FLAGS) $(VDO_ROOT) > prepare.out

$(SOURCES): prepare
	[ -e  $(WORK_DIR)/$(PACKAGE)/dm-vdo-target.c ] || mv $(WORK_DIR)/$(PACKAGE)/dm-vdo/dm-vdo-target.c $(WORK_DIR)/$(PACKAGE)
	mkdir -p $(SOURCES)
	tar -C $(WORK_DIR) -zcvf $(SOURCES)/$(PACKAGE).tgz ./$(PACKAGE)
	cp $(WORK_DIR)/vdo-$(VDO_VERSION).tgz $(SOURCES)

SRPM =	mkdir -p $(SPECS);					\
	cp $(WORK_DIR)/$(1) $(SPECS) && cd $(SPECS) &&		\
	$(RPMBUILD) $(RPMBUILD_FLAG) $(RPMBUILD_ARGS) $(1)

.PHONY: vdo-srpm
vdo-srpm: $(SOURCES)
	$(call SRPM,vdo.spec)

.PHONY: kvdo-srpm
kvdo-srpm: $(SOURCES)
	$(call SRPM,kvdo.spec)

.PHONY: srpms
srpms:
	$(MAKE) vdo-srpm
	$(MAKE) kvdo-srpm

.PHONY: test-srpms
test-srpms:
	make VDO_ROOT=$(TEST_ROOT) srpms
