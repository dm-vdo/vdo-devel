# VDO C fake linux Makefile
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$

SRC_DIR = ../../../..
include ../../defines

INCLUDES  = -I$(VDO_BASE_DIR) -isystem $(VDO_FAKE_DIR) $(UDS_INCLUDES)
CFLAGS 	  = -fPIC $(GLOBAL_CFLAGS) $(INCLUDES) -Wno-write-strings \
	    -DINTERNAL

SOURCES = $(wildcard *.c)
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all
all: $(OBJECTS)

.PHONY: doc
doc:

.PHONY: checkin
checkin:

.PHONY: clean
clean:
	rm -f *.a *.so *.o core*
	rm -fr $(DEPDIR) tmpdir

########################################################################
# Dependency processing

%.o: %.c
	$(COMPILE.c) -MD -MF $(DEPDIR)/$*.d.new -MP -MT $@ -o $@ $<
	if cmp -s $(DEPDIR)/$*.d $(DEPDIR)/$*.d.new ;		 \
	then							 \
		$(RM) $(DEPDIR)/$*.d.new ;			 \
	else							 \
		$(MV) $(DEPDIR)/$*.d.new $(DEPDIR)/$*.d ;	 \
	fi

$(DEPDIR)/%.d: %.c $(FAKE_INCLUDES)
	@mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -M -MF $@ -MP -MT $*.o $<

ifneq ($(MAKECMDGOALS),clean)
DEPSOURCES = $(wildcard *.c)
-include $(DEPSOURCES:%.c=$(DEPDIR)/%.d)
endif
