# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright 2023 Red Hat
#

TOP_DIR      = ../..
PLATFORM_DIR = ..

include $(TOP_DIR)/defines
include $(PLATFORM_DIR)/defines

.PHONY:	all
all:	$(UDS_PACKAGE)

.PHONY:	clean
clean:
	rm -fr $(UDS_DIR) *.tgz
###############################################################################
# Build rules for the uds DKMS tarball object

UDS_DIR			= $(basename $(UDS_PACKAGE))
UDS_SUBDIR		= $(UDS_DIR)/$(UDS_NAME)
UDS_DKMS_OBJECTS	= $(notdir $(UDS_DKMS_SOURCES:%.c=%.o))
TEST_MODULES            = y

UDS_DKMS_EXTRA = -DSTATIC=static

UDS_DKMS_SOURCES = $(UDS_COMMON_SOURCES)			\
	$(addprefix $(K_UDS_DIR)/,	logger.c		\
					memory-alloc.c		\
					request-queue.c		\
					thread-cond-var.c	\
					thread-device.c		\
					thread-registry.c	\
					uds-module.c		\
					uds-sysfs.c		\
					uds-threads.c)

UDS_DKMS_HEADERS =						\
	$(wildcard $(K_UDS_DIR)/*.h)				\
	$(wildcard $(SRC_UDS_DIR)/*.h)

# If VDO_PRESERVE_DKMS is defined then leave the intermediate products
ifndef VDO_PRESERVE_DKMS
.DELETE_ON_ERROR:
endif
$(UDS_PACKAGE): $(UDS_DKMS_SOURCES) $(UDS_DKMS_HEADERS)
	rm -fr $(UDS_DIR) $@
	mkdir -p $(UDS_SUBDIR)
	mkdir -p $(UDS_DIR)/linux
	cp -p $(UDS_DKMS_SOURCES) $(UDS_SUBDIR)
	cp -p $(UDS_DKMS_HEADERS) $(UDS_SUBDIR)
	$(call DKMS_MAKEFILE, $(UDS_NAME), $(UDS_DKMS_OBJECTS),	\
		$(UDS_DKMS_EXTRA)) >$(UDS_SUBDIR)/Makefile
	echo obj-m += $(UDS_NAME)/ >$(UDS_DIR)/Makefile
	$(call DKMS_CONF, $(UDS_NAME), $(VDO_MARKETING_VERSION), $(UDS_NAME)) \
		>$(UDS_DIR)/dkms.conf
	tar cvfz $@ --owner=0 --group=0 --mode=a+rX $(UDS_DIR)
ifeq (y,$(TEST_MODULES))
	$(MAKE) -C $(KERNEL_SOURCE_DIR) M=$(abspath $(UDS_DIR)) modules \
		$(if $(LLVM), $(LLVM_ARGS),)
endif
