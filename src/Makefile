# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#

SRC_DIR   = .
CURR_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

include $(SRC_DIR)/defines
include $(CURRENT_VERSION_FILE)
export VDO_VERSION

SUBDIRS                 := perl c++ python tools
CLEAN_SUBDIRS           := $(SUBDIRS) stats packaging
CHECK_SUBDIRS           := perl tools python/vdo
CHECKIN_SUBDIRS         := c++ perl python packaging
VDO_CHECKIN_SUBDIRS     := c++ perl

# The directory of build products to be picked up by tagAndArchive.pl
ARCHIVE_DIR = $(SRC_DIR)/../archive
ARCHIVE     = $(SRC_DIR)/packaging/github
LOGBASE     = $(CURDIR)/../logs

# Number of concurrent jobs to run
NUM_JOBS := $(shell expr $$(grep -F -c processor /proc/cpuinfo) + 1)

# Create a unique test index name based on the current directory's name,
# for the jenkins target.
TEST_INDEX := /u1/zubenelgenubi$(subst /,_,$(CURR_DIR))

.PHONY: all
all::
	set -e;					\
	for i in $(SUBDIRS); do			\
		$(MAKE) -C $$i all || exit 1;	\
	done

.PHONY: clean
clean::
	set -e;					\
	for i in $(CLEAN_SUBDIRS); do		\
		$(MAKE) -C $$i clean || exit 1;	\
	done
	$(RM) -rf $(ARCHIVE_DIR)

.PHONY: cleanall
cleanall::
	rm -rf $(LOGBASE)
	set -e;						\
	for i in $(CLEAN_SUBDIRS); do			\
		$(MAKE) -C $$i cleanall || exit 1;	\
	done
	$(RM) -rf $(ARCHIVE_DIR)

.PHONY: checkin
checkin::
	set -e;						\
	for i in $(CHECKIN_SUBDIRS); do			\
		$(MAKE) -C $$i checkin || exit 1;	\
	done

.PHONY: vdo-checkin
vdo-checkin::
	set -e;						\
	for i in $(VDO_CHECKIN_SUBDIRS); do		\
		$(MAKE) -C $$i vdo-checkin || exit 1;	\
	done

.PHONY: check
check:
	set -e;					\
	for i in $(CHECK_SUBDIRS); do		\
		$(MAKE) -C $$i check;		\
	done

.PHONY: check-style
check-style:
	$(MAKE) -C $(KPATCH_DIR) KERNEL_VERSION=$(CHECK_PATCH_KERNEL_VERSION) \
	  clean $@

.PHONY: udstests
udstests:
	mkdir -p $(LOGBASE)/$@
	$(MAKE) -C perl/udstest SAVELOGS=1 LOGDIR=$(LOGBASE)/$@ jenkins

.PHONY: vdotests
vdotests:
	mkdir -p $(LOGBASE)/$@
	$(MAKE) -C perl/vdotest SAVELOGS=1 LOGDIR=$(LOGBASE)/$@ jenkins

.PHONY: test-vdo-distribution
test-vdo-distribution:
	mkdir -p $(LOGBASE)/$@
	$(MAKE) -C perl/vdotest SAVELOGS=1 LOGDIR=$(LOGBASE)/$@ $@

.PHONY: vdotests-release
vdotests-release:
	mkdir -p $(LOGBASE)/$@
	$(MAKE) -C perl/vdotest \
	  SAVELOGS=1 LOGDIR=$(LOGBASE)/$@ checkin platform

.PHONY: cunittests
cunittests:
	mkdir -p $(LOGBASE)/cunit
	$(MAKE) -C c++ SAVELOGS=1 LOGDIR=$(LOGBASE)/cunit vdo-checkin

.PHONY: cunittests-release
cunittests-release:
	mkdir -p $(LOGBASE)/cunit-release
	$(MAKE) -C c++ SAVELOGS=1 LOGDIR=$(LOGBASE)/cunit-release \
	  checkin

.PHONY: jenkins
jenkins:
	$(MAKE) -j$(NUM_JOBS) jenkins-parallel

.PHONY: jenkins-parallel
jenkins-parallel: export VDO_TESTINDEX=$(TEST_INDEX)
jenkins-parallel: clean
	$(MAKE) all
	@echo "Using index name of $$VDO_TESTINDEX"
	$(MAKE) cunittests udstests vdotests check #XXX vdotests-release
	$(MAKE) archive
	$(MAKE) check-style
	$(MAKE) test-vdo-distribution

.PHONY: archive
archive:
	$(MAKE) -C $(ARCHIVE)
	mkdir -p $(ARCHIVE_DIR)
	find $(ARCHIVE) -type f -name "*.rpm" -exec cp {} $(ARCHIVE_DIR) \;
	$(MAKE) -C $(ARCHIVE) clean

.PHONY: cleanall
cleanall::
	rm -f ID TAGS id-exclusions cscope.*

.PHONY: FORCE
FORCE:

.PHONY: tags
tags: TAGS

.PHONY: id
id-exclusions: FORCE
	-rm -f $@
	test -d packaging/rpm && echo packaging/rpm >> $@
	# exclude all third-party source directories
	for d in $(SUBDIRS); do \
	  test -d $$d/third && echo $$d/third >> $@; \
	done
	# exclude everything but {VDO,...}Test subdirectories
	find perl/vdotest -mindepth 1 -maxdepth 1 -type d \
	  \! \( -name "*Test" -o -name "*~" \) >> $@

ID:	id-exclusions
	mkid --prune="`cat id-exclusions`" . \
		       /permabit/build/common/lastrun/perl/lib/Permabit

TAGS:	ID
	fnid | etags -
