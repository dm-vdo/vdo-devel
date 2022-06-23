# VDO C tools Makefile
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$
#

SRC_DIR = ../../..
include $(SRC_DIR)/c++/defines

BINDIR    = ../bin
CFLAGS 	  = -fPIC $(GLOBAL_CFLAGS) -Wno-write-strings -DINTERNAL
LDFLAGS   = $(GLOBAL_LDFLAGS)
LDPRFLAGS = -ldl -pthread -lz -lrt -lm

# To add a new program X, add a new variable X_OBJS here with its dependencies
# and add X to the variable PROGS

PROGS =	checkerboardWrite     \
	genDataBlocks         \
	genDiscard            \
	murmur3collide	      \
	setReadOnly
BIN_PROGS = $(addprefix $(BINDIR)/, $(PROGS))

LOGDIR ?= $(CURDIR)

.PHONY: all
all: $(BIN_PROGS)

.PHONY: doc
doc:

.PHONY: clean
clean:
	rm -f  $(USERLIB) *.so *.o core*
	rm -fr $(DEPDIR) tmpdir
	rm -f  $(PROGS) $(BIN_PROGS)

########################################################################
# Dependency processing

%.o: %.c
	$(COMPILE.c) -MMD -MF $(DEPDIR)/$*.d.new -MP -MT $@ -o $@ $<
	if cmp -s $(DEPDIR)/$*.d $(DEPDIR)/$*.d.new ;		 \
	then							 \
		$(RM) $(DEPDIR)/$*.d.new ;			 \
	else							 \
		$(MV) $(DEPDIR)/$*.d.new $(DEPDIR)/$*.d ;	 \
	fi

$(DEPDIR)/%.d: %.c
	@mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -MM -MF $@ -MP -MT $*.o $<

.SECONDEXPANSION:
# This rule builds all binaries X, with dependencies of X_OBJS and PROGS_LIBS.
# For instance, this rule builds vdoAudit from vdoAudit_OBJS and PROGS_LIBS.
$(PROGS): $$@.o $$($$@_OBJS)
	@echo "Building $@ from $^"
	$(CC) $(LDFLAGS) $^ $(LDPRFLAGS) -o $@

# For program X, make $(BINDIR)/X depend on X, and install X into $(BINDIR).
$(BIN_PROGS): $$(notdir $$@)
	install $< $@

ifneq ($(MAKECMDGOALS),clean)
DEPSOURCES = $(wildcard *.c)
-include $(DEPSOURCES:%.c=$(DEPDIR)/%.d)
endif
