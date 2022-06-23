# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$

SRC_DIR = ../..
include $(SRC_DIR)/c++/defines

CFLAGS	 = $(GLOBAL_CFLAGS) -Wno-write-strings
LDFLAGS	 = $(GLOBAL_LDFLAGS)

PROGS = fsync

all: $(PROGS)

clean:
	rm -f $(PROGS) *.o
	rm -rf $(DEPDIR)

doc:

fsync: fsync.o
	$(CC) $(LDFLAGS) $^ -o $@

########################################################################
# Dependency processing

%.o: %.c
	$(COMPILE.c) -MMD -MF $(DEPDIR)/$*.d.new -MP -MT $@ -o $@ $<
	if cmp -s $(DEPDIR)/$*.d $(DEPDIR)/$*.d.new; then \
		$(RM) $(DEPDIR)/$*.d.new ; \
	else \
		$(MV) $(DEPDIR)/$*.d.new $(DEPDIR)/$*.d ; \
	fi

$(DEPDIR)/%.d: %.c
	@mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -MM -MF $@ -MP -MT $*.o $<

ifneq ($(MAKECMDGOALS),clean)
SOURCES = $(wildcard *.c)
-include $(SOURCES:%.c=$(DEPDIR)/%.d)
endif

########################################################################

.PHONY: all clean doc
