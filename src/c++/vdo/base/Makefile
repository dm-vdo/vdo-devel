# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright 2023 Red Hat
#
# VDO base C++ Makefile
#

SRC_DIR = ../../..
include ../defines
include Makefile.common

INCLUDES = -I. $(UDS_INCLUDES) -I$(VDO_KERNEL_DIR) -isystem $(VDO_FAKE_DIR)
CFLAGS   = -fPIC $(GLOBAL_CFLAGS) $(INCLUDES) -Wno-write-strings \
	   -DVDO_VERSION="\"$(VDO_VERSION)\"" -DINTERNAL -DRHEL_INTERNAL \
           -DSTATIC=
LDFLAGS  = $(GLOBAL_LDFLAGS)

OBJECTS := $(sort $(BASE_OBJS) $(USER_OBJECTS))

.PHONY: all
all: libvdo.a libvdouser.a

.PHONY: checkin
checkin: all

.PHONY: clean
clean:
	rm -f *.o *.a *.so
	rm -rf $(DEPDIR)

libvdo.a: $(BASE_OBJS) Makefile.common
	$(RM) $@
	$(AR) cr $@ $(BASE_OBJS)

libvdouser.a: $(USER_OBJECTS) Makefile.common
	$(RM) $@
	$(AR) cr $@ $(USER_OBJECTS)

$(DEPDIR)/%-test.d: %.c 
	@mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -M -MF $@ -MP -MT $*.o $<

%.o: %.c
	$(COMPILE.c) -MD -MF $(DEPDIR)/$*.d.new -MP -MT $@ -o $@ $<
	if cmp -s $(DEPDIR)/$*.d $(DEPDIR)/$*.d.new ;     \
	then                                              \
		$(RM) $(DEPDIR)/$*.d.new ;                \
	else                                              \
		$(MV) $(DEPDIR)/$*.d.new $(DEPDIR)/$*.d ; \
	fi

$(DEPDIR)/%.d: %.c $(FAKE_INCLUDES)
	@mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -M -MF $@ -MP -MT $*.o $<


ifneq ($(MAKECMDGOALS),clean)
DEPSOURCES = $(OBJECTS:%.o=%.c)
-include $(DEPSOURCES:%.c=$(DEPDIR)/%.d)
endif
