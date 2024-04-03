#
# %COPYRIGHT%
#
# %LICENSE%
#

SUBDIRS = monitor

.PHONY: all clean install
all clean install:
	for d in $(SUBDIRS); do         \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
