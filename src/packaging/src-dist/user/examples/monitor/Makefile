#
# %COPYRIGHT%
#
# %LICENSE%
#

INSTALLFILES=monitor_check_vdostats_logicalSpace.pl	\
	monitor_check_vdostats_physicalSpace.pl		\
	monitor_check_vdostats_savingPercent.pl


INSTALL = install
INSTALLOWNER ?= -o root -g root
defaultdocdir ?= /usr/share/doc
name ?= vdo

INSTALLDIR=$(DESTDIR)/$(defaultdocdir)/$(name)/examples/monitor

.PHONY: all clean install
all:;

clean:;

install:
	$(INSTALL) $(INSTALLOWNER) -d $(INSTALLDIR)
	for i in $(INSTALLFILES); do \
	  $(INSTALL) $(INSTALLOWNER) -m 755 $$i $(INSTALLDIR); \
	done
