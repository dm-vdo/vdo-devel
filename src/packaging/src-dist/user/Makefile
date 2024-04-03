#
# %COPYRIGHT%
#
# %LICENSE%
#

INSTALL = install
INSTALLOWNER ?= -o root -g root
defaultdocdir ?= /usr/share/doc
name ?= vdo
INSTALLDIR=$(DESTDIR)/$(defaultdocdir)/$(name)

SUBDIRS = examples utils

.PHONY: all clean install
all clean:
	for d in $(SUBDIRS); do         \
	  $(MAKE) -C $$d $@ || exit 1; \
	done

install:
	$(INSTALL) $(INSTALLOWNER) -d $(INSTALLDIR)
	$(INSTALL) $(INSTALLOWNER) -m 644 COPYING $(INSTALLDIR)
	for d in $(SUBDIRS); do         \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
