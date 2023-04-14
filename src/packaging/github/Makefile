# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#

VDO_ROOT ?= $(realpath ../../..)

SRC_DIR = ../..
include ../defines
include $(SRC_DIR)/defines
CURRENT_VERSION_FILE := $(VDO_ROOT)/src/tools/installers/CURRENT_VERSION
include $(CURRENT_VERSION_FILE)

ifdef AUTHOR
  AUTHOR_FLAG = --author=$(AUTHOR)
endif

ifdef CHANGELOG
  CHANGELOG_FLAGS = --changelog=$(CHANGELOG) $(AUTHOR_FLAG)
endif

PREPARE_GITHUB_DIST ?= $(SRC_DIR)/perl/bin/prepareGitHubDist.pl

WORK_DIR          ?= $(realpath .)/work
MANIFEST          ?= src/packaging/github/MANIFEST.yaml
REPOSITORY_PREFIX  = dm-vdo
RELEASE_DIR        = /permabit/release
PACKAGE       = kvdo-$(VDO_VERSION)
PREPARED_DIR  = $(WORK_DIR)/$(PACKAGE)

# For building RPMs for testing
RPMBUILD   =  rpmbuild
BUILD_DIR ?= $(realpath .)/build
SOURCES    = $(BUILD_DIR)/SOURCES
SPECS      = $(BUILD_DIR)/SPECS

CLEAN_FILES = $(WORK_DIR)                    \
              $(BUILD_DIR)                   \
              prepareGitHubDist.out

ifdef CHANGELOG_FLAGS
  ifdef RPM_DIR
    export DIST_TARGET = from-rpms
  else
    export DIST_TARGET = no-rpms
  endif
else
    export DIST_TARGET = rpms
endif

.PHONY: all
all: $(DIST_TARGET)

.PHONY: clean
clean:
	rm -rf $(CLEAN_FILES)

$(WORK_DIR):
	mkdir -p $(WORK_DIR)

prepareGitHubDist.out: $(WORK_DIR)
	$(PREPARE_GITHUB_DIST) --dest=$(WORK_DIR) \
          --repositoryPrefix=$(REPOSITORY_PREFIX) $(FLAGS) $(EXTRA_FLAGS) \
	  | tee $@
	[ -e  $(WORK_DIR)/$(PACKAGE)/dm-vdo-target.c ] || mv $(WORK_DIR)/$(PACKAGE)/dm-vdo/dm-vdo-target.c $(WORK_DIR)/$(PACKAGE)

.PHONY: prepare
prepare: FLAGS=--test --manifest=$(MANIFEST) $(VDO_ROOT)
prepare: prepareGitHubDist.out

.PHONY: install
install:

# This target remains as a synonym of prepare both to make the transition
# easier on developers and because mergeToGitHub.pl uses it.
.PHONY: test
test: prepare

.PHONY: kvdo-test
kvdo-test: prepare
	cd $(WORK_DIR)/kvdo-* && $(MAKE) -C /usr/src/kernels/`uname -r` M=`pwd`

.PHONY: vdo-test
vdo-test: prepare
	$(MAKE) -C $(WORK_DIR)/vdo*

.PHONY: test-build
test-build: kvdo-test vdo-test

# rpmbuild fails to clean up after itself on RHEL8 if BUILDROOT is under
# BUILD_DIR in NFS so just pick a temporary local directory for it to use.
BUILDROOT := $(shell mktemp -du BUILDROOT-XXXXXXXXXX --tmpdir)

.PHONY: tarball
tarball:
	cd $(WORK_DIR) && tar -czf $(SOURCES)/$(TO_TAR).tgz $(TO_TAR)

$(SOURCES):
	mkdir -p $(SOURCES)
	$(MAKE) TO_TAR=$(notdir $(wildcard $(WORK_DIR)/vdo*)) tarball
	$(MAKE) TO_TAR=$(notdir $(wildcard $(WORK_DIR)/kvdo*)) tarball

.PHONY: rpm
rpm: $(SOURCES)
	mkdir -p $(SPECS)
	cp $(WORK_DIR)/*/*.spec $(SPECS)
	cd $(SPECS) && $(RPMBUILD) -ba \
	  --define '_topdir $(realpath $(BUILD_DIR))' \
	  --buildroot=$(BUILDROOT) $(SPEC)

.PHONY: vdo-rpm
vdo-rpm: SPEC=vdo.spec
vdo-rpm: rpm

.PHONY: kvdo-rpm
kvdo-rpm: SPEC=kvdo.spec
kvdo-rpm: rpm

# This needs to copy the prepared sources into the RPM build directory
# before we do the test build since we don't have a good way to clean
# up the build products, and they would get in the way of the RPM build.
.PHONY: rpms
rpms: clean prepare
	$(MAKE) $(SOURCES)
	$(MAKE) kvdo-test
	$(MAKE) vdo-rpm
	$(MAKE) kvdo-rpm

# This remains as a synonym of rpms to make the transition easier on
# developers.
.PHONY: test-rpms
test-rpms: rpms

.PHONY: from-rpms
from-rpms: clean
from-rpms: FLAGS=--fromRPMs $(CHANGELOG_FLAGS) $(RPM_DIR)
from-rpms: prepareGitHubDist.out

.PHONY: no-rpms
no-rpms:
	$(error "RPM_DIR must be set when specifying a changelog")

# Parallel builds are risky since all of the targets here are a linear
# pipeline.
.NOTPARALLEL:
