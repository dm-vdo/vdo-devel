# kernel devices top level Makefile
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$
#

SRC_DIR = ../..
include $(SRC_DIR)/c++/defines

KERNEL_MODULES = pbitcorruptor-1.tgz pbitfua-1.tgz pbitdory-1.tgz \
                  pbittracer-1.tgz

.PHONY: all
all: $(KERNEL_MODULES)

.PHONY: clean
clean:
	rm -f  $(KERNEL_MODULES)

.PHONY: test
test:
	set -e;				\
	for i in `ls -d */`; do		\
	  make -C $$i modules clean;	\
	done;

pbitcorruptor-1.tgz: $(addprefix corruptor/, \
		dkms.conf Makefile.dkms Makefile.common)	\
		$(filter-out corruptor/pbitcorruptor%, $(wildcard corruptor/*.[ch])) \
		bioIterator.h \
		$(wildcard common.[ch])
	tar cvfz $@ \
		--transform s,Makefile.dkms,Makefile, \
		--transform s,corruptor/,pbitcorruptor-1/, 	\
		--transform s,^bioIterator,pbitcorruptor-1/bioIterator,	\
		--transform s,^common,pbitcorruptor-1/common,	\
		--show-transformed-names 		\
		$^

pbitfua-1.tgz:	$(addprefix fua/, \
	          dkms.conf Makefile.dkms Makefile.common)	\
		$(filter-out fua/pbitfua%, $(wildcard fua/*.[ch])) \
		$(wildcard common.[ch])
	tar cvfz $@ \
		--transform s,Makefile.dkms,Makefile,		\
		--transform s,fua/,pbitfua-1/, 	\
		--transform s,^common,pbitfua-1/common,	\
		--show-transformed-names 		\
		$^

pbitdory-1.tgz:	$(addprefix dory/, \
	          dkms.conf Makefile.dkms Makefile.common )	\
		$(filter-out dory/pbitdory%, $(wildcard dory/*.[ch])) \
		$(wildcard common.[ch])
	tar cvfz $@ \
		--transform s,Makefile.dkms,Makefile,	\
		--transform s,dory/,pbitdory-1/, 	\
		--transform s,^common,pbitdory-1/common,	\
		--show-transformed-names 		\
		$^

pbittracer-1.tgz: $(addprefix tracer/, \
		dkms.conf Makefile.dkms Makefile.common Makefile.murmur )	\
		$(filter-out tracer/pbittracer%, $(wildcard tracer/*.[ch])) \
		$(wildcard tracer/murmur/*.[ch])  \
		bioIterator.h \
		$(wildcard common.[ch])
	tar cvfz $@ \
		--transform s,Makefile.dkms,Makefile, \
		--transform s,tracer/,pbittracer-1/, 	\
		--transform s,^bioIterator,pbittracer-1/bioIterator,	\
		--transform s,^common,pbittracer-1/common,	\
		--show-transformed-names 		\
		$^
