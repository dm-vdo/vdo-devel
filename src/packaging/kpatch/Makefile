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

#
# make prepare:
#
#   This prepares UDS and VDO sources for adding to the linux sources.
#   This will leave massaged source files in
#   work/kvdo-$(VDO_MARKETING_VERSION)/vdo.
#
# make overlay:
#
#   This will copy the massaged sources into the linux source tree and
#   generate a VDO Makefile. A Kconfig file will be added and linked
#   into drivers/md/Kconfig. Documentation files will be dropped into
#   place. And the VDO directory will be added to
#   drivers/md/Makefile. It does not attempt to clean the linux source
#   tree before doing its work; the 'linuxclean' target will do that.
#
# make kpatch:
#
#   This will generate the patch itself.
#
# Linux source tree maintenance targets:
#
#   linux_clone will clone the tree from GitHub into $(LINUX_SRC).
#   linux_update will update the tree
#   linux_clean will reset the tree to its pre-vdo state.
#   

VDO_ROOT ?= $(realpath ../../..)
SRC_DIR = ../..
include ../defines
include $(SRC_DIR)/defines

CURRENT_VERSION_FILE := $(VDO_ROOT)/src/tools/installers/CURRENT_VERSION
include $(CURRENT_VERSION_FILE)

VDO_DOC=$(VDO_ROOT)/doc/dm-vdo.rst

CHANGE_LOG ?= Update $(VDO_VERSION)

LINUX_SRC ?= /u1/GitHub/linux

ifndef KERNEL_VERSION
KERNEL_VERSION := $(shell \
	perl -e 'while (<>) { \
	  $$version = $$1 if (/^VERSION = (\d+)/m); \
	  $$patchlevel = $$1 if (/^PATCHLEVEL = (\d+)/m); \
	  $$sublevel = $$1 if (/^SUBLEVEL = (\d+)/m); } \
	$$kernel_version = "$$version.$$patchlevel.$$sublevel"; \
	$$kernel_version =~ /\d+\.\d+\.\d+/ or die; \
	print $$kernel_version;' <$(LINUX_SRC)/Makefile || echo NONE)
endif

GIT=git
GITARGS=

WORK_DIR ?= $(realpath .)/work
PREPARED_DIR = $(WORK_DIR)/kvdo-$(VDO_VERSION)/vdo
MANIFEST ?= src/packaging/kpatch/MANIFEST.yaml
PREPARE_GITHUB_DIST ?= $(SRC_DIR)/perl/bin/prepareGitHubDist.pl
CHECKPATCH_TYPES := ERROR BRACES CODE_INDENT CONST_STRUCT UNNECESSARY_ELSE
CHECKPATCH_TYPES += TRAILING_WHITESPACE
CHECKPATCH_REGRESSIONS = $(shell echo '$(CHECKPATCH_TYPES)' | tr ' ' '|')

#
# Location of the Linux distribution and subdirectories of interest
#
LINUX_GITHUB_URL=https://github.com/torvalds/linux
LINUX_DOC_SRC=$(LINUX_SRC)/Documentation/admin-guide/device-mapper
LINUX_MD_SRC=$(LINUX_SRC)/drivers/md
LINUX_VDO_SRC=$(LINUX_MD_SRC)/dm-vdo

CLEAN_FILES = prepare.out checkpatch.out $(WORK_DIR)

.PHONY: all
all:
	@echo To generate a kernel patch, do make prepare, make check-style,
	@echo make overlay, and make kpatch in that order.

.PHONY: clean
clean:
	$(RM) -r $(CLEAN_FILES)

prepare: prepare.out

prepare.out: $(WORK_DIR)
	[ "$(KERNEL_VERSION)" != "NONE" ] || exit 1; \
	$(PREPARE_GITHUB_DIST) --dest=$(WORK_DIR) \
	  --manifest=$(MANIFEST) --kernel=$(KERNEL_VERSION) $(VDO_ROOT) \
	  | tee $@

$(WORK_DIR):
	mkdir -p $(WORK_DIR)

SPDX := \# SPDX-License-Identifier: GPL-2.0-only

# Check that no ERROR-level problems or fixed WARNING level problems
# are re-introduced.
check-style: checkpatch.out
	! grep -q -E '$(CHECKPATCH_REGRESSIONS)' checkpatch.out

# Ignore NEW_TYPEDEFS because some of our typedefs are justifiable
CHECKPATCH_IGNORE = NEW_TYPEDEFS
CHECKPATCH_ARGS := --no-tree --file --terse --show-types --color=never
CHECKPATCH_ARGS += --ignore $(CHECKPATCH_IGNORE)

checkpatch.out: prepare.out
	-$(CHECKPATCH) $(CHECKPATCH_ARGS) $(PREPARED_DIR)/*.[hc] \
	  | tee $@



.PHONY: overlay overlay_vdo
overlay overlay_vdo:
	mkdir -p $(LINUX_VDO_SRC)
	cp -r $(PREPARED_DIR)/* $(LINUX_VDO_SRC)
	cd  $(LINUX_VDO_SRC) \
		&& echo '$(SPDX)' >Makefile \
		&& echo >>Makefile \
		&& echo 'obj-$$(CONFIG_DM_VDO) += dm-vdo.o' >>Makefile \
		&& echo >>Makefile \
		&& echo -n 'ccflags-y := -I$$(src)' >>Makefile \
		&& echo ' -DCURRENT_VERSION=\"$(VDO_VERSION)\"' >>Makefile \
		&& echo >>Makefile \
		&& echo -n 'dm-vdo-objs := ' >>Makefile \
		&& echo *.c | sed -e 's/\.c /.o \\\n\t/g' \
				  -e 's/\.c/.o/' >>Makefile
	cp dm-vdo_Kconfig $(LINUX_VDO_SRC)/Kconfig
	grep -q DM_VDO $(LINUX_MD_SRC)/Makefile || \
		sed -i '/dm-verity\.o/ a obj-$$(CONFIG_DM_VDO)            += dm-vdo/' $(LINUX_MD_SRC)/Makefile
	grep -q drivers.md.dm-vdo.Kconfig $(LINUX_MD_SRC)/Kconfig || \
	  sed -i '/config DM_CACHE$$/ i source "drivers/md/dm-vdo/Kconfig"\n' $(LINUX_MD_SRC)/Kconfig
	cp -f $(VDO_DOC) $(LINUX_DOC_SRC)/

#
# The following are git operations that work on the linux source tree
# in $(LINUX_SRC).
#
.PHONY: linux_clone
linux_clone:
	cd $(LINUX_SRC)/.. && $(GIT) $(GITARGS) clone --depth 1 $(LINUX_URL)

.PHONY: linux_update
linux_update:
	cd $(LINUX_SRC) && $(GIT) $(GITARGS) pull

.PHONY: linux_clean
linux_clean:
	cd $(LINUX_SRC) && $(GIT) $(GITARGS) reset --hard origin/master \
	&& $(GIT) $(GITARGS) clean -f .

.PHONY: kpatch
kpatch:
	cd $(LINUX_SRC) && $(GIT) $(GITARGS) add . \
	  && $(GIT) $(GITARGS) commit -m "$(CHANGE_LOG)" \
	  && $(GIT) $(GITARGS) format-patch -s -o .. HEAD ^origin

# Parallel builds are risky since all of the targets here are a linear
# pipeline.
.NOTPARALLEL:
