#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$

INSTALLFILES= \
        adaptlvm.8               \
        vdoaudit.8               \
	vdodebugmetadata.8       \
        vdodmeventd.8            \
        vdodumpblockmap.8        \
        vdodumpconfig.8          \
	vdodumpmetadata.8        \
	vdoforcerebuild.8        \
	vdoformat.8              \
	vdolistmetadata.8        \
	vdoreadonly.8            \
	vdorecover.8             \
	vdoregenerategeometry.8  \
	vdosetuuid.8             \
        vdostats.8

INSTALL = install
INSTALLOWNER ?= -o root -g root
mandir ?= /usr/man

INSTALLDIR=$(DESTDIR)/$(mandir)

.PHONY: all clean install
all:;

clean:;

install:
	$(INSTALL) $(INSTALLOWNER) -d $(INSTALLDIR)/man8
	for i in $(INSTALLFILES); do		\
	  $(INSTALL) $(INSTALLOWNER) -m 644 $$i $(INSTALLDIR)/man8;	\
	done
