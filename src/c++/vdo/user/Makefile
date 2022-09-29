# VDO C user Makefile
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$

SRC_DIR = ../../..
include ../defines

INCLUDES  = -I$(VDO_BASE_DIR) $(UDS_INCLUDES)
CFLAGS 	  = -fPIC $(GLOBAL_CFLAGS) $(INCLUDES) -Wno-write-strings \
	    -DINTERNAL -DCURRENT_VERSION="\"$(VDO_VERSION)\""

STATS_HEADER = $(VDO_BASE_DIR)/statistics.h
STATS_FILES  = messageStatsReader.c \
               vdoStatsWriter.c
STATS_OBJS   = $(STATS_FILES:.c=.o)

LDFLAGS   = $(GLOBAL_LDFLAGS)
LDPRFLAGS = -ldl -pthread -lz -lrt -lm -luuid
LDSHFLAGS = -shared -fPIC

LIBVDO = $(VDO_BASE_DIR)/libvdouser.a
LIBUDS = $(UDS_USER_BUILD_DIR)/libuds.a
LIBFLAGS = -pthread -lrt

USER_OBJS = blockMapUtils.o     \
            fileLayer.o         \
            parseUtils.o        \
            slabSummaryReader.o \
            userVDO.o           \
            vdoConfig.o         \
            vdoVolumeUtils.o

PROGS = corruptPBNRef          \
        udsCalculateSize       \
        vdoDMEventd            \
        vdoDumpConfig          \
	vdoFillIndex           \
        vdoForceRebuild        \
        vdoFormat              \
        vdoInitializeBlockMap  \
        vdoSetUUID             \
        vdoStats               \
        vdoWarmup              \
        $(SUPPORT_PROGS)

SUPPORT_PROGS = vdoAudit               \
                vdoDebugMetadata       \
                vdoDumpBlockMap        \
                vdoDumpMetadata        \
                vdoListMetadata        \
                vdoReadOnly            \
                vdoRegenerateGeometry

ALL_PROGS = $(PROGS)

DOWNCASE = $(shell echo $(1) | tr A-Z a-z)

BIN_CAMELCASE_PROGS = $(addprefix $(VDO_BIN_DIR)/, $(ALL_PROGS))
BIN_LOWERCASE_PROGS = $(addprefix $(VDO_BIN_DIR)/, $(call DOWNCASE,$(ALL_PROGS)))
BIN_PROGS := $(BIN_CAMELCASE_PROGS) $(BIN_LOWERCASE_PROGS)

DMEVENT_SRC = dmeventd_vdo
DMEVENT_LIB = libdevmapper-event-lvm2vdo.so

LOGDIR ?= $(CURDIR)

.PHONY: all
all: $(STATS_HEADER) $(BIN_PROGS) $(DMEVENT_LIB)

$(STATS_HEADER) $(STATS_FILES): $(VDO_STATS_DEPS)
	$(MAKE) -C $(VDO_STATS_DIR)

$(LIBUDS):
	$(MAKE) -C $(UDS_USER_DIR)

$(LIBVDO):
	$(MAKE) -C $(VDO_BASE_DIR) libvdouser.a

.PHONY: doc
doc:

.PHONY: checkin
checkin:
	$(MAKE) -C man checkin

.PHONY: clean
clean:
	rm -f *.a *.so *.o core*
	rm -fr $(PROFDIR) $(DEPDIR) tmpdir
	rm -f  $(ALL_PROGS)
	rm -f  $(BIN_PROGS)
	rm -f  $(DMEVENT_LIB)
	rm -f  $(STATS_FILES)
	$(MAKE) -C man clean

libuser.a: $(USER_OBJS)
	$(RM) $@
	$(AR) cr $@ $(USER_OBJS)

$(DMEVENT_LIB): $(DMEVENT_SRC).o $(LIBVDO) $(LIBUDS) $(DMEVENT_SRC).vers
	$(CC) $(LDFLAGS) $(LDSHFLAGS) $(LDPRFLAGS) $(DMEVENT_SRC).o $(LIBVDO) \
	  $(LIBUDS) -Wl,--version-script,$(DMEVENT_SRC).vers -o $@

########################################################################
# Dependency processing

%.o: %.c
	$(COMPILE.c) -MMD -MF $(DEPDIR)/$*.d.new -MP -MT $@ \
		-MT "\$$(PROFDIR)/$@" -o $@ $<
	if cmp -s $(DEPDIR)/$*.d $(DEPDIR)/$*.d.new ;		 \
	then							 \
		$(RM) $(DEPDIR)/$*.d.new ;			 \
	else							 \
		$(MV) $(DEPDIR)/$*.d.new $(DEPDIR)/$*.d ;	 \
	fi

vdoDMEventd.o: CFLAGS += "-DPLUGIN_NAME=\"$(DMEVENT_LIB)\""

$(DEPDIR)/%.d: %.c $(STATS_HEADER) $(LIBUDS)
	@mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -MM -MF $@ -MP -MT $*.o -MT "\$$(PROFDIR)/$*.o" $<

.SECONDEXPANSION:
$(PROGS): $$@.o libuser.a $(LIBVDO) $(LIBUDS)
	echo "Building $@ from $^"
	$(CC) $(LDFLAGS) $^ $(LDPRFLAGS) -o $@

vdoStats: $$@.o $(STATS_OBJS) libuser.a $(LIBVDO) $(LIBUDS)

vdoFormat: LDPRFLAGS += "-lblkid"
vdoDMEventd: LDPRFLAGS += "-ldevmapper-event"

# For program X, make $(VDO_BIN_DIR)/X depend on X, and install X into
# $(VDO_BIN_DIR).
$(BIN_CAMELCASE_PROGS): $$(notdir $$@)
	install -s $< $@

# Install under the all-lower-case name too, which will eventually be the
# only name we want to use.
define LOWERCASE_TEMPLATE
$(VDO_BIN_DIR)/$(call DOWNCASE,$(1)) : $(1)
	install -s $(1) $(VDO_BIN_DIR)/$(call DOWNCASE,$(1))
endef
$(foreach PROG,$(ALL_PROGS), $(eval $(call LOWERCASE_TEMPLATE,$(PROG))))

ifneq ($(MAKECMDGOALS),clean)
DEPSOURCES = $(wildcard *.c)
-include $(DEPSOURCES:%.c=$(DEPDIR)/%.d)
endif
