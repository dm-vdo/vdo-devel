#
# %COPYRIGHT%
#
# %LICENSE%
#

INSTALL = install
INSTALLOWNER ?= -o root -g root
name ?= vdo
defaultdocdir ?= /usr/share/doc
defaultlicensedir ?= /usr/share/licenses
DOCDIR=$(DESTDIR)/$(defaultdocdir)/$(name)
LICENSEDIR=$(DESTDIR)/$(defaultlicensedir)/$(name)

SUBDIRS = examples utils

.PHONY: all clean install
all clean:
	for d in $(SUBDIRS); do         \
	  $(MAKE) -C $$d $@ || exit 1; \
	done

install:
	$(INSTALL) $(INSTALLOWNER) -d $(DOCDIR)
	$(INSTALL) $(INSTALLOWNER) -D -m 644 COPYING -t $(LICENSEDIR) 
	for d in $(SUBDIRS); do         \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
