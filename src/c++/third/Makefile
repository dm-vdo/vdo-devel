# $Id$


SUBDIRS = fio

all clean:
	set -e;				\
	for d in $(SUBDIRS); do		\
		$(MAKE) -C $$d $@;	\
	done


doc:

.PHONY: all clean doc
