# VDO C fake library Makefile
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$

SRC_DIR = ../../..
include ../defines

SOURCES = $(wildcard */*.c)
OBJECTS = $(SOURCES:.c=.o)

SUBDIRS = linux

.PHONY: all
all: libvdofake.a

.PHONY: checkin
checkin:

.PHONY: clean
clean:
	rm -f *.a
	for d in $(SUBDIRS); do 		\
		$(MAKE) -C $$d $@;		\
	done

libvdofake.a: $(SOURCES) $(FAKE_INCLUDES)
	$(RM) $@
	for d in $(SUBDIRS); do	\
	  $(MAKE) -C $$d;       \
        done
	$(AR) cr $@ $(OBJECTS)
