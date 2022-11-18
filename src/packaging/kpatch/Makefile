# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
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

CHECKPATCH_FIXED := \
	ERROR \
	BLOCK_COMMENT_STYLE \
	BRACES \
	CODE_INDENT \
	CONST_STRUCT \
	CONSTANT_COMPARISON \
	CVS_KEYWORD \
	DEEP_INDENTATION \
	FUNCTION_ARGUMENTS \
	LEADING_SPACE \
	LINE_SPACING \
	MINMAX \
	PREFER_DEFINED_ATTRIBUTE_MACRO \
	PRINTK_WITHOUT_KERN_LEVEL \
	REPEATED_WORD \
	RETURN_PARENTHESES \
	SSCANF_TO_KSTRTO \
	SPACE_BEFORE_TAB \
	SPDX_LICENSE_TAG \
	TRAILING_WHITESPACE \
	UNNECESSARY_ELSE \
	WHITESPACE_AFTER_LINE_CONTINUATION \
	YIELD

CHECKPATCH_REGRESSIONS = $(shell echo '$(strip $(CHECKPATCH_FIXED))' \
							| tr ' ' '|')

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
.PHONY: check-style
check-style: checkpatch checkincludes

checkpatch: checkpatch.out
	! grep -E '$(CHECKPATCH_REGRESSIONS)' checkpatch.out

# CHECKPATCH_IGNORE contains types we have no immediate intention of fixing.
# Ignore NEW_TYPEDEFS because some of our typedefs are justifiable
CHECKPATCH_IGNORE_TYPES = NEW_TYPEDEFS \
						  PREFER_PR_LEVEL \
						  VSPRINTF_SPECIFIER_PX
CHECKPATCH_IGNORE = $(shell echo '$(strip $(CHECKPATCH_IGNORE_TYPES))' \
						| tr ' ' ',')
CHECKPATCH_ARGS := --no-tree --file --terse --show-types --color=never
CHECKPATCH_ARGS += --ignore $(CHECKPATCH_IGNORE)

CHECKPATCH_STRICT_TYPES = BRACES
CHECKPATCH_STRICT = $(shell echo '$(strip $(CHECKPATCH_STRICT_TYPES))' \
						| tr ' ' ',')
CHECKPATCH_STRICT_ARGS := $(CHECKPATCH_ARGS) --strict
CHECKPATCH_STRICT_ARGS += --types $(CHECKPATCH_STRICT)

checkpatch.out: prepare.out
	-$(CHECKPATCH) $(CHECKPATCH_ARGS) $(PREPARED_DIR)/*.[hc] \
	  | tee $@
	-$(CHECKPATCH) $(CHECKPATCH_STRICT_ARGS) $(PREPARED_DIR)/*.[hc] \
	  | tee -a $@

checkincludes.out: prepare.out
	$(CHECKINCLUDES) $(PREPARED_DIR)/*.[hc] | tee checkincludes.out

checkincludes: checkincludes.out
	@grep "No duplicate includes found" $<

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
