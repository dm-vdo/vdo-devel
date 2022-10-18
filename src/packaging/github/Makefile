#
# Copyright (c) 2017, 2018, 2021 Red Hat, Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
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

VDO_ROOT ?= $(realpath ../../..)

SRC_DIR = ../..
include ../defines
include $(SRC_DIR)/defines

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

# For building RPMs for testing
RPMBUILD   =  rpmbuild
BUILD_DIR ?= $(realpath .)/build
SOURCES    = $(BUILD_DIR)/SOURCES
SPECS      = $(BUILD_DIR)/SPECS

CLEAN_FILES = $(WORK_DIR)                 \
              $(BUILD_DIR)                \
              prepareGitHubCheckpatch.out \
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

.PHONY: prepare
prepare: FLAGS=--test --manifest=$(MANIFEST) $(VDO_ROOT)
prepare: prepareGitHubDist.out

# Ignore NEW_TYPEDEFS because some of our typedefs are justifiable
CHECKPATCH_IGNORE = NEW_TYPEDEFS
CHECKPATCH_ARGS := --no-tree --file --terse --show-types --color=never
CHECKPATCH_ARGS += --ignore $(CHECKPATCH_IGNORE)

prepareGitHubCheckpatch.out: prepare
# For now, ignore checkpatch errors so that this is purely advisory.
	-$(CHECKPATCH) $(CHECKPATCH_ARGS) $(WORK_DIR)/kvdo-*/vdo/*.[hc] \
	  | tee $@

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

# Generate the advisory checkpatch.pl output and then process it to check
# that no ERROR-level problems were found and that already addressed issues
# are not re-introduced.
.PHONY: check-style
check-style: prepareGitHubCheckpatch.out
	@ ! grep ERROR prepareGitHubCheckpatch.out
	@ ! grep BRACES prepareGitHubCheckpatch.out
	@ ! grep CODE_INDENT prepareGitHubCheckpatch.out
	@ ! grep CONST_STRUCT prepareGitHubCheckpatch.out
	@ ! grep UNNECESSARY_ELSE prepareGitHubCheckpatch.out

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
