# Makefile for building the kernel module
#
# %COPYRIGHT%
#
# %LICENSE%
#

src ?= .

KERNEL_SRC_DIR ?=  /lib/modules/$(shell uname -r)/build
SUDO := $(shell test $$(id -u) = 0 || echo sudo)

include $(src)/Makefile.common

INCLUDES += -I$(LOSSY_DIR)

MODNAME = pbitlossy

EXTRA_CFLAGS += $(INCLUDES)

obj-m := $(MODNAME).o
$(MODNAME)-objs := $(LOSSY_OBJS)

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

.PHONY: all clean check TAGS tags
