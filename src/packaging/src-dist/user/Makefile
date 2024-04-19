#
# %COPYRIGHT%
#
# %LICENSE%
#

INSTALL = install
INSTALLOWNER ?= -o root -g root
defaultdocdir ?= /usr/share/doc
name ?= vdo
licensedir ?= $(defaultdocdir)
LICENSEDIR=$(DESTDIR)/$(licensedir)/$(name)
INSTALLDIR=$(DESTDIR)/$(defaultdocdir)/$(name)

SUBDIRS = examples utils

.PHONY: all clean install
all clean:
	for d in $(SUBDIRS); do         \
	  $(MAKE) -C $$d $@ || exit 1; \
	done

install:
	$(INSTALL) $(INSTALLOWNER) -d $(INSTALLDIR)
	$(INSTALL) $(INSTALLOWNER) -D -m 644 COPYING -t $(LICENSEDIR) 
	for d in $(SUBDIRS); do         \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
