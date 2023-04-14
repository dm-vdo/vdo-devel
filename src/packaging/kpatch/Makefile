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
# make check-style:
#
#   This checks the prepared sources against the kernel coding style
#   using checkpatch.pl and checkincludes.pl from the kernel scripts
#   directory.
#
# make overlay:
#
#   This copies the massaged sources and the documentation into the
#   linux source tree and updates drivers/md/Kconfig and
#   drivers/md/Makefile. It does not attempt to clean the linux source
#   tree before doing its work; the 'linux_clean' target will do that.
#
# make kpatch:
#
#   This generates the patch itself.
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

ifndef KERNEL_VERSION
ifdef LINUX_SRC
KERNEL_VERSION := $(shell \
	perl -e 'while (<>) { \
	  $$version = $$1 if (/^VERSION = (\d+)/m); \
	  $$patchlevel = $$1 if (/^PATCHLEVEL = (\d+)/m); \
	  $$sublevel = $$1 if (/^SUBLEVEL = (\d+)/m); } \
	$$kernel_version = "$$version.$$patchlevel.$$sublevel"; \
	$$kernel_version =~ /\d+\.\d+\.\d+/ or die; \
	print $$kernel_version;' <$(LINUX_SRC)/Makefile || echo NONE)
CHECKINCLUDES := $(LINUX_SRC)/scripts/checkincludes.pl
CHECKPATCH := $(LINUX_SRC)/scripts/checkpatch.pl
else
KERNEL_VERSION = $(shell uname -r)
endif
endif

# kpatch and overlay need a full kernel source tree, prepare and
# check-style only need KERNEL_VERSION
ifneq ($(filter overlay kpatch, $(MAKECMDGOALS)),)
ifndef LINUX_SRC
$(error The overlay and kpatch goals require LINUX_SRC to be the path to a kernel source tree)
endif
endif

GIT=git
GITARGS=

WORK_DIR ?= $(realpath .)/work
PREPARED_DIR = $(WORK_DIR)/kvdo-$(VDO_VERSION)/dm-vdo
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
	LOGICAL_CONTINUATIONS \
	MEMORY_BARRIER \
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
# Location of the Linux source tree and subdirectories of interest
#
LINUX_GITHUB_URL=https://github.com/torvalds/linux
LINUX_DOC_SRC=$(LINUX_SRC)/Documentation/admin-guide/device-mapper
LINUX_MD_SRC=$(LINUX_SRC)/drivers/md
LINUX_VDO_SRC=$(LINUX_MD_SRC)/dm-vdo

ifdef LINUX_SRC
LINUX_BRANCH = $(shell cd $(LINUX_SRC) && $(GIT) branch --show-current) 
endif

CLEAN_FILES = prepare.out checkpatch.out checkincludes.out $(WORK_DIR)

.PHONY: all
all:
	@echo To generate a kernel patch, do make prepare, make check-style,
	@echo make overlay, and make kpatch in that order.

.PHONY: clean
clean:
	$(RM) -rf $(CLEAN_FILES)

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

CHECKPATCH_STRICT_TYPES = BRACES \
			  LOGICAL_CONTINUATIONS
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
	rm -rf $(LINUX_VDO_SRC)
	mkdir -p $(LINUX_VDO_SRC)
	cp -r $(PREPARED_DIR)/* $(LINUX_VDO_SRC)
	sed -i -E -e 's/(#define	CURRENT_VERSION).*/\1 "$(VDO_VERSION)"/' \
		$(LINUX_VDO_SRC)/dm-vdo-target.c
	mv $(LINUX_VDO_SRC)/dm-vdo-target.c $(LINUX_MD_SRC)
	mv $(LINUX_VDO_SRC)/linux/murmurhash3.h $(LINUX_SRC)/include/linux/

	# Update Kconfig and Makefile against the master branch, not
	# a different branch such as the vdo-devel branch
	cd $(LINUX_SRC) && \
		git restore --source origin/master $(LINUX_MD_SRC)/Kconfig && \
		git restore --source origin/master $(LINUX_MD_SRC)/Makefile

	grep -q DM_VDO $(LINUX_MD_SRC)/Makefile || \
		patch -p3 $(LINUX_MD_SRC)/Makefile <Makefile.patch

	grep -q DM_VDO $(LINUX_MD_SRC)/Kconfig || \
		patch -p3 $(LINUX_MD_SRC)/Kconfig <Kconfig.patch

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

.PHONY: linux-clean linux_clean
linux-clean linux_clean:
	cd $(LINUX_SRC) && \
	  $(GIT) reset --hard origin/$(LINUX_BRANCH) && \
	  $(GIT) clean -f .

.PHONY: kpatch
kpatch:
	cd $(LINUX_SRC) && $(GIT) $(GITARGS) add . \
	  && $(GIT) $(GITARGS) commit -m "$(CHANGE_LOG)" \
	  && $(GIT) $(GITARGS) format-patch -s -o .. HEAD ^origin

# Parallel builds are risky since all of the targets here are a linear
# pipeline.
.NOTPARALLEL:
