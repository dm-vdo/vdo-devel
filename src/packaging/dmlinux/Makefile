# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#

SRC_DIR = ../..
include ../defines

RPMBUILD      = rpmbuild
RPMBUILD_FLAG = -bs
BUILD_DIR    ?= $(realpath .)/build
WORK_DIR     ?= $(realpath .)/work
PACKAGE       = kvdo-$(VDO_VERSION)
PREPARED_DIR  = $(WORK_DIR)/$(PACKAGE)
SOURCES       = $(BUILD_DIR)/SOURCES

SPECS         = $(BUILD_DIR)/SPECS

# rpmbuild fails to clean up after itself on RHEL8 if BUILDROOT is under
# BUILD_DIR in NFS so just pick a temporary local directory for it to use.
BUILDROOT := $(shell mktemp -du BUILDROOT-XXXXXXXXXX --tmpdir)

RPMBUILD_ARGS = --define '_topdir $(realpath $(BUILD_DIR))' \
		--buildroot=$(BUILDROOT)

.PHONY: all
all: kvdo-srpm

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(WORK_DIR) prepare.out

prepare: prepare.out

.PHONY: FORCE
prepare.out: FORCE
	$(if $(DMLINUX_SOURCE_DIR),,$(error Must set DMLINUX_SOURCE_DIR))
	rm -rf $(WORK_DIR)
	mkdir -p $(WORK_DIR)/$(PACKAGE)
	sed -e "s/@VERSION@/$(VDO_VERSION)/g" kvdo.spec >$(WORK_DIR)/kvdo.spec
	cp kernel/Makefile $(WORK_DIR)/$(PACKAGE)
	cd $(DMLINUX_SOURCE_DIR)/drivers/md && 		\
	find dm-vdo -type f -name '*.[ch]'		\
	-exec cp --parents {} $(WORK_DIR)/$(PACKAGE) ';' > prepare.out

$(SOURCES): prepare
	mkdir -p $(SOURCES)
	tar -C $(WORK_DIR) -zcvf $(SOURCES)/$(PACKAGE).tgz ./$(PACKAGE)

SRPM =	mkdir -p $(SPECS);					\
	cp $(WORK_DIR)/$(1) $(SPECS) && cd $(SPECS) &&		\
	$(RPMBUILD) $(RPMBUILD_FLAG) $(RPMBUILD_ARGS) $(1)


.PHONY: kvdo-srpm
kvdo-srpm: $(SOURCES)
	$(call SRPM,kvdo.spec)

