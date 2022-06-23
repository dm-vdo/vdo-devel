# $Id$

SUBDIRS       = 
CLEAN_DIRS    = $(SUBDIRS)
CHECK_SUBDIRS = 

.PHONY: all
all:
	set -e; for dir in $(SUBDIRS); do \
	  $(MAKE) -C $$dir $@; \
	done

.PHONY: check
check:
	set -e; for dir in $(CHECK_SUBDIRS); do \
	  $(MAKE) -C $$dir $@; \
	done

.PHONY: clean cleanall
clean cleanall:
	for dir in $(CLEAN_DIRS); do \
	  $(MAKE) -C $$dir clean; \
	done
	rm -f *.changes *.deb

.PHONY: checkin doc
