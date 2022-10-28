# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#
# This Makefile assumes that the kernel sources are installed in the
# default place.
#

SRC_DIR      = ../../..
VDO_TOP      = ..
VDO_BASE_DIR = $(VDO_TOP)/base

include $(SRC_DIR)/defines
include $(VDO_TOP)/defines
include $(CURRENT_VERSION_FILE)
include $(VDO_BASE_DIR)/Makefile.common
include Makefile.common

VDO_KERNEL_VERSION ?= $(shell uname -r)
KERNEL_SOURCE_DIR ?= /lib/modules/$(VDO_KERNEL_VERSION)/build

UDS_KERNEL_DIR       = $(UDS_TOP)/kernelLinux
UDS_KERNEL_BUILD_DIR = $(UDS_KERNEL_DIR)/build
UDS_DKMS_DIR         = uds-$(VDO_MARKETING_VERSION)
UDS_DKMS_TGZ         = $(UDS_KERNEL_BUILD_DIR)/$(UDS_DKMS_DIR).tgz
UDS_EXCLUDES         = --exclude=uds-module.c
UDS_SRCS := $(notdir $(shell tar zxvf $(UDS_DKMS_TGZ) '*.c' $(UDS_EXCLUDES)))
UDS_OBJS := $(UDS_SRCS:.c=.o)

UDS_KERNEL_FILES =					\
	$(wildcard $(UDS_SRC_DIR)/*.[ch])		\
	$(wildcard $(UDS_SRC_DIR)/*/*.[ch])		\
	$(wildcard $(UDS_KERNEL_DIR)/uds/*.[ch])

ALL_OBJS = $(BASE_OBJS) $(KERNEL_OBJS) $(UDS_OBJS)

STATS_FILES = messageStats.c

KERNEL_SOURCES = $(KERNEL_OBJS:.o=.c)

# The arguments to pass to the kernel make scripts to build an out of tree
# # module with clang. The config options are to force not passing
# # -mrecord-mcount, which clang seems to dislike though nominally supporting.
LLVM_ARGS = LLVM=1                              \
            CONFIG_FTRACE_MCOUNT_USE_CC=        \
            CONFIG_FTRACE_MCOUNT_USE_OBJTOOL=y

DKMS_DIR = $(VDO_MODNAME)-$(VDO_MARKETING_VERSION)
DKMS_TGZ = $(DKMS_DIR).tgz
VDO_SUBDIR = $(DKMS_DIR)/$(VDO_MODNAME)

.PHONY: all
all: $(DKMS_TGZ)

# This rule uses a separate Makefile so that we can include defines
# without worrying about the care which this Makefile makes about
# definitions (see comments above).
$(STATS_FILES): $(VDO_STATS_DEPS)
	$(MAKE) -f Makefile.statistics

$(UDS_DKMS_TGZ): $(UDS_KERNEL_FILES)
	$(MAKE) -C $(UDS_KERNEL_DIR)

DKMS_FILES =	$(addprefix $(VDO_BASE_DIR)/,$(KERNEL_SOURCES))		\
		$(addprefix $(VDO_BASE_DIR)/,$(BASE_OBJS:%.o=%.c))	\
		$(sort $(wildcard *.h))			                \
		$(filter-out $(VDO_BASE_DIR)/list.h,			\
			$(wildcard $(VDO_BASE_DIR)/*.h))

.DELETE_ON_ERROR:
$(DKMS_TGZ): $(UDS_DKMS_TGZ) $(DKMS_FILES) \
		Makefile.module.in Makefile.common ../base/Makefile.common
	$(MAKE) clean-dkms-dirs
	mkdir -p $(VDO_SUBDIR)
	tar zxvf $(UDS_DKMS_TGZ) $(UDS_EXCLUDES)
	mv $(UDS_DKMS_DIR)/uds/*.[hc] $(VDO_SUBDIR)
	mv $(UDS_DKMS_DIR)/linux $(DKMS_DIR)
	@echo Append to Makefile
	echo obj-m += $(VDO_MODNAME)/ >>$(DKMS_DIR)/Makefile
	@echo Replace dkms.conf
	$(call DKMS_CONF,$(VDO_MODNAME),$(VDO_MARKETING_VERSION), \
		$(VDO_MODNAME)) >$(DKMS_DIR)/dkms.conf
	sed -e "s/@CURRENT_VERSION@/$(VDO_VERSION)/g"	\
	    -e "s/@MODNAME@/$(VDO_MODNAME)/g"		\
	    Makefile.module.in >$(VDO_SUBDIR)/Makefile
	cat Makefile.common >>$(VDO_SUBDIR)/Makefile
	cat ../base/Makefile.common >>$(VDO_SUBDIR)/Makefile
	echo UDS_OBJS=$(UDS_OBJS) >>$(VDO_SUBDIR)/Makefile
	cp -p $(DKMS_FILES) $(VDO_SUBDIR)
	tar cvfz $@ --owner=0 --group=0 --mode=a+rX-w $(DKMS_DIR)
	$(MAKE) -C $(KERNEL_SOURCE_DIR) M=$(abspath $(DKMS_DIR)) modules \
		$(if $(V),V=1,) $(if $(LLVM),$(LLVM_ARGS),)
	$(if $(V), echo Report the greatest stack usage within our module.,)
	$(if $(V), objdump -d $(VDO_SUBDIR)/kvdo.ko | $(KERNEL_SOURCE_DIR)/scripts/checkstack.pl,)
	$(if $(V),:,$(MAKE) clean-dkms-dirs)

.PHONY: build-against-latest-kernel
build-against-latest-kernel:
	$(MAKE) $(DKMS_TGZ) KERNEL_SOURCE_DIR=/permabit/not-backed-up/kernels/mainline/

.PHONY: check-kernel-style
check-kernel-style:
	$(CHECKPATCH) $(CHECKPATCH_ARGS) *.[hc]

.PHONY: fix-kernel-style
fix-kernel-style:
	-$(CHECKPATCH) --fix $(CHECKPATCH_ARGS) *.[hc]

.PHONY: check-kernel-doc
check-kernel-doc:
	$(KERNEL_SOURCE_DIR)/scripts/kernel-doc -none -v $(KERNEL_SOURCES)

.PHONY: check-clang-format
check-clang-format:
	@echo "This does not work on RHEL7!"
	for d in $(KERNEL_SOURCES); do		\
		clang-format $$d > /tmp/$$d;	\
		diff /tmp/$$d $$d;		\
		rm /tmp/$$d;			\
	done
	@echo "This does not work on RHEL7!"

.PHONY: clean clean-dkms-dirs
clean: clean-dkms-dirs
	rm -f *.log *.tar *.tgz $(STATS_FILES)
	rm -f $(CHECKPATCH_FILES)

clean-dkms-dirs:
	if [ -e $(DKMS_DIR) ]; then chmod -fR +w $(DKMS_DIR); fi
	if [ -e $(UDS_DKMS_DIR) ]; then chmod -fR +w $(UDS_DKMS_DIR); fi
	rm -fr $(DKMS_DIR) $(UDS_DKMS_DIR)

doc:

###############################################################################
# Create the contents of a DKMS conf file.
#
# Argument 1:  The DKMS package name
# Argument 2:  The DKMS package version
# Argument 3:  The 1st kernel module name
# Argument 4:  The 2nd kernel module name (optional)
# Argument 5:  The 3rd kernel module name (optional)
#
# XXX - This code is copied from //eng/uds/kernelLinux/defines.
#       It would be nice to have only a single copy.

DKMS_CONF =	(echo PACKAGE_NAME=\"$(strip $(1))\";	\
		echo PACKAGE_VERSION=\"$(strip $(2))\";	\
		echo MODULE_NAME=\"$(strip $(1))\";	\
		echo AUTOINSTALL=\"yes\";		\
		echo BUILD_DEPENDS=LZ4_COMPRESS;	\
		echo BUILD_DEPENDS=LZ4_DECOMPRESS;	\
		$(call DKMS_MODULE,0,$(strip $(3)))	\
		$(call DKMS_MODULE,1,$(strip $(4)))	\
		$(call DKMS_MODULE,2,$(strip $(5))))

DKMS_MODULE = $(if $(2),						\
	echo;								\
	echo BUILT_MODULE_NAME[$(1)]=\"$(2)\";				\
	echo BUILT_MODULE_LOCATION[$(1)]=\"$(2)\";			\
	echo DEST_MODULE_LOCATION[$(1)]=\"/kernel/drivers/block/\";	\
	echo STRIP[$(1)]=\"no\";)
