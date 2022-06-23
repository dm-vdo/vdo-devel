# Makefile for building the kernel module
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$
#

src ?= .

KERNEL_SRC_DIR ?=  /lib/modules/$(shell uname -r)/build
SUDO := $(shell test $$(id -u) = 0 || echo sudo)

include $(src)/../Makefile.devices
include $(src)/Makefile.common
include $(src)/Makefile.murmur

INCLUDES += -I$(TRACER_DIR)

CFLAGS_super.o := -DDEBUG

MODNAME = pbittracer

EXTRA_CFLAGS += $(INCLUDES)

obj-m := $(MODNAME).o
$(MODNAME)-objs := $(TRACER_OBJS) $(MURMUR_OBJS) $(DEVICE_COMMON_OBJS)

all: modules

modules:
	$(MAKE) -C $(KERNEL_SRC_DIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_SRC_DIR) M=$(CURDIR) clean
	rm -f ID TAGS

check:;

TAGS tags:
	mkid
	fnid | etags -

test: modules
	if lsmod | grep $(MODNAME) > /dev/null; then \
                 $(SUDO) rmmod $(MODNAME); \
	fi
	$(SUDO) modprobe dm_mod
	$(SUDO) insmod $(MODNAME).ko

.PHONY: all clean check murmur TAGS tags

###############################################################################
# Override kernel-object build rule when sources are in another directory

DEVICE_COMMON_OBJS_FULL_PATH = $(addprefix $(obj)/,$(DEVICE_COMMON_OBJS))
$(DEVICE_COMMON_OBJS_FULL_PATH): $(obj)/%.o: $(DEVICE_COMMON_DIR)/%.c
	$(call cmd,force_checksrc)
	$(call if_changed_rule,cc_o_c)

MURMUR_OBJS_FULL_PATH = $(addprefix $(obj)/,$(MURMUR_OBJS))
$(MURMUR_OBJS_FULL_PATH): $(obj)/%.o: $(MURMUR_DIR)/%.c
	$(call cmd,force_checksrc)
	$(call if_changed_rule,cc_o_c)
