#
# %COPYRIGHT%
#
# %LICENSE%
#

SUBDIRS = corruptor tracer

.PHONY: all check checkin clean dist doc
all check checkin clean dist doc:
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done
