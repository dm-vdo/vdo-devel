# Makefile for auto-generating statistics structures, generators, and parsers.
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$
#

SUBDIRS       = vdo
CLEAN_SUBDIRS = $(addsuffix .clean, $(SUBDIRS))

.PHONY: all
all: $(SUBDIRS)

.PHONY: $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@

.PHONY: clean
clean: $(CLEAN_SUBDIRS)

.PHONY: cleanall
cleanall: clean

%.clean:
	$(MAKE) -C $(basename $@) clean
