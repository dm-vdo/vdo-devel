# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#
# make prepare:
#
#   This prepares VDO and UDS sources for adding to the linux source
#   tree.  This will leave massaged source files in
#   work/kvdo-$(VDO_MARKETING_VERSION)/vdo.
#

WD := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

VDO_ROOT ?= $(realpath ../../..)
SRC_DIR = ../..
CURRENT_VERSION_FILE := $(VDO_ROOT)/src/tools/installers/CURRENT_VERSION
include $(CURRENT_VERSION_FILE)

WORK_DIR ?= $(WD)/work
PREPARED_DIR = $(WORK_DIR)/kvdo-$(VDO_VERSION)
MANIFEST ?= src/packaging/patchset/MANIFEST.yaml
PREPARE_GITHUB_DIST ?= $(SRC_DIR)/perl/bin/prepareGitHubDist.pl

LINUX_URL ?= https://git.kernel.org/pub/scm/linux/kernel/git/device-mapper/linux-dm.git
LINUX_BRANCH ?= for-next
LINUX_SRC ?= $(WD)/linux
LINUX_DRIVERS_MD = $(LINUX_SRC)/drivers/md
LINUX_DM_VDO = $(LINUX_DRIVERS_MD)/dm-vdo
LINUX_DOC = $(LINUX_SRC)/Documentation/admin-guide/device-mapper

ifneq ($(filter overlay patchset,$(MAKECMDGOALS)),)
KERNEL_VERSION := $(shell \
	perl -e 'while (<>) { \
	  $$version = $$1 if (/^VERSION = (\d+)/m); \
	  $$patchlevel = $$1 if (/^PATCHLEVEL = (\d+)/m); \
	  $$sublevel = $$1 if (/^SUBLEVEL = (\d+)/m); } \
	$$kernel_version = "$$version.$$patchlevel.$$sublevel"; \
	$$kernel_version =~ /\d+\.\d+\.\d+/ or die; \
	print $$kernel_version;' <$(LINUX_SRC)/Makefile || echo NONE)
endif

.PHONY: all
all:
	@echo run "make prepare" to process the dm-vdo source.
	@echo run "make patchset" to generate a patch set.

.PHONY: clean
clean:
	rm -rf prepare.out $(WORK_DIR) 0*.patch

prepare: prepare.out

prepare.out: $(LINUX_SRC) $(WORK_DIR)
	$(PREPARE_GITHUB_DIST) --dest=$(WORK_DIR) \
	  --manifest=$(MANIFEST) --kernel=$(KERNEL_VERSION) $(VDO_ROOT) \
	  | tee $@
	sed -E -i -e 's/(#define	CURRENT_VERSION).*/\1 "$(VDO_VERSION)"/' \
		$(PREPARED_DIR)/md/dm-vdo-target.c

$(WORK_DIR):
	mkdir -p $(WORK_DIR)

# Copy list of files to Linux tree subdir, add, and commit.
# 1 - list of files
# 2 - destination directory
# 3 - file containing the commit message
COMMIT = \
	cp -r $(1) $(2) && \
	cd $(LINUX_SRC) && git add . && git commit -s -F $(3)

UDS_FILES = $(PREPARED_DIR)/uds/*.[hc] 
VDO_FILES = $(PREPARED_DIR)/vdo/*.[hc]
TARGET_FILE = $(PREPARED_DIR)/md/dm-vdo-target.c
DOC_FILES = $(PREPARED_DIR)/vdo.rst $(PREPARED_DIR)/vdo-design.rst 
BUILD_FILES = $(WD)/src/Kconfig $(WD)/src/Makefile
COVER_LETTER = $(PREPARED_DIR)/coverLetter.txt

.PHONY: patchset
patchset: prepare
	cd $(LINUX_SRC) && git checkout $(LINUX_BRANCH)
	mkdir -p $(LINUX_DM_VDO)
	$(call COMMIT, $(UDS_FILES),$(LINUX_DM_VDO),$(WD)/src/uds.txt)
	$(call COMMIT, $(VDO_FILES),$(LINUX_DM_VDO),$(WD)/src/vdo.txt)
	$(call COMMIT, $(TARGET_FILE), $(LINUX_DRIVERS_MD),$(WD)/src/target.txt)
	$(call COMMIT, $(BUILD_FILES), $(LINUX_DRIVERS_MD),$(WD)/src/build.txt)
	$(call COMMIT, $(DOC_FILES), $(LINUX_DOC),$(WD)/src/doc.txt)
	cd $(LINUX_SRC) && GIT_EDITOR='cp $(COVER_LETTER) ' git branch --edit-description  
	cd $(LINUX_SRC) && git format-patch -s -o .. --cover-letter --cover-from-description=subject HEAD ^origin/$(LINUX_BRANCH)

GIT_CONFIG = --config "http.sslVerify=false"

.PHONY: clone
clone:
	git clone $(GIT_CONFIG) --depth 1 --no-single-branch $(LINUX_URL) $(LINUX_SRC)

.PHONY: linux-clean
linux-clean:
	cd $(LINUX_SRC) && \
	  git reset --hard origin/$(LINUX_BRANCH) && git clean -f .
