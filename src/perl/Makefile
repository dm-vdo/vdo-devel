# Makefile for all perl modules
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$

LIB             := $(CURDIR)/lib
COMMONLIB       := $(CURDIR)/commonlib
DEFAULT_DESTDIR := $(CURDIR)/build
DESTDIR	         = $(DEFAULT_DESTDIR)

SUBDIRS          = vdotest
CHECKIN_SUBDIRS  = vdotest
SYMLINKS         = Permabit
DOCTREES         = Permabit       \
                   bin            \
                   statsGenerator \
                   vdotest

PERL_COMMON_DEFINE   =  /etc/permabit/common-defines
PERL_COMMON_DEFINE_P :=  $(shell test -f $(PERL_COMMON_DEFINE) && echo y)

ifeq "$(PERL_COMMON_DEFINE_P)" "y"
  include $(PERL_COMMON_DEFINE)

  ifndef COMMONDIR
    $(error COMMONDIR definition not found in $(PERL_COMMON_DEFINE))
  endif

  ifeq "$(shell test -d $(COMMONDIR) && echo y)" "y"
    COMMON_CMD = ln -snf $(COMMONDIR) $(COMMONLIB)
  else
    $(error COMMONDIR points at an invalid location '$(COMMONDIR)')
  endif
endif


all:	$(LIB) $(SUBDIRS) symlinks

# Add all dirs in $(SUBDIRS) here

$(LIB): $(COMMONLIB)
	mkdir -p $@

# Create the COMMONLIB symlink pointing at COMMONDIR, but only on systems in the
# VDO lab.
$(COMMONLIB):
	$(COMMON_CMD)

symlinks:	$(LIB)
	set -e; for link in $(SYMLINKS); do \
	  ln -snf ../$$link $(LIB)/$$link; \
	done

doc:
	for dir in $(DOCTREES); do \
	  mkdir -p $(DESTDIR)/$$dir; \
	  $(MAKE) -C $$dir $@ DESTDIR=$(DESTDIR)/$$dir; \
	done
	genPdocIndex.pl $(DESTDIR)

man:
	for dir in $(DOCTREES); do \
	  $(MAKE) -C $$dir $@; \
	done

check:  symlinks do-check

do-check:
	for dir in $(DOCTREES); do \
	  $(MAKE) -C $$dir check || exit 1; \
	done

doc-clean:
	for dir in $(DOCTREES); do \
	  $(MAKE) -C $$dir clean; \
	done
	rm -rf $(DEFAULT_DESTDIR)

dir-clean: doc-clean
	for dir in $(SUBDIRS); do \
	  $(MAKE) -C $$dir clean; \
	done
	for link in $(SYMLINKS); do \
	  rm -rf $(LIB)/$$link; \
	done

dir-cleanall: doc-clean
	for dir in $(SUBDIRS); do \
	  $(MAKE) -C $$dir cleanall; \
	done
	for link in $(SYMLINKS); do \
	  rm -rf $(LIB)/$$link; \
	done

cleanlogs:
	for dir in $(CHECKIN_SUBDIRS); do \
	  $(MAKE) -C $$dir cleanlogs; \
	done

lib-clean:
	rm -rf $(LIB)
	rm -f $(COMMONLIB)

cleanall: lib-clean dir-cleanall

clean: lib-clean dir-clean

checkin: symlinks
	for dir in $(CHECKIN_SUBDIRS); do \
	  $(MAKE) -C $$dir checkin; \
	done

vdo-checkin: symlinks
	$(MAKE) -C vdotest checkin

test-vdo-tarball: symlinks
	$(MAKE) -C vdotest test-vdo-tarball

TAGS:
	find . -name '*.p[lm]' -print | etags -

.PHONY: all doc symlinks check checkin vdo-checkin TAGS critic
.PHONY: clean all-clean lib-clean dir-clean doc-clean do-check do-critic
.PHONY:	test-vdo-tarball
