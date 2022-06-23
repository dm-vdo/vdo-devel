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
#   It uses the tools and MANIFEST files in ../github, to reduce
#   duplication and avoid having to update yet another manifest when
#   changing souce files. This will leave massaged source files in
#   ../github/work/kvdo-$(VDO_MARKETING_VERSION)/vdo.
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

GITHUB_PACKAGING_DIR=$(realpath ..)/github
GITHUB_WORK_DIR=$(GITHUB_PACKAGING_DIR)/work/kvdo-$(VDO_VERSION)

#
# Location of the Linux distribution and subdirectories of interest
#
LINUX_GITHUB_URL=https://github.com/torvalds/linux
LINUX_DOC_SRC=$(LINUX_SRC)/Documentation/admin-guide/device-mapper
LINUX_MD_SRC=$(LINUX_SRC)/drivers/md
LINUX_VDO_SRC=$(LINUX_MD_SRC)/dm-vdo

.PHONY: all
all:;

.PHONY: clean
clean:;

.PHONY: prepare
prepare:
	[ "$(KERNEL_VERSION)" != "NONE" ] || exit 1; \
	$(MAKE) -C $(GITHUB_PACKAGING_DIR) clean;  \
	$(MAKE) -C $(GITHUB_PACKAGING_DIR) prepare \
		EXTRA_FLAGS="--kernel=$(KERNEL_VERSION)"

SPDX := \# SPDX-License-Identifier: GPL-2.0-only

.PHONY: overlay overlay_vdo
overlay overlay_vdo:
	mkdir -p $(LINUX_VDO_SRC)
	cp -r $(GITHUB_WORK_DIR)/vdo/* $(LINUX_VDO_SRC)
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
