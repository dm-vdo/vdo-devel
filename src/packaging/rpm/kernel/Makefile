VDO_VERSION = %%VDOVersion%%

OBJECTS = $(patsubst %.c,dm-vdo/%.o,$(notdir $(wildcard $(src)/dm-vdo/*.c)))

INCLUDES = -I$(src)/dm-vdo

ccflags-y = -std=gnu11						\
	    -fno-builtin-memset					\
	    -fno-omit-frame-pointer				\
	    -fno-optimize-sibling-calls				\
	    -Werror						\
	    -Wmissing-declarations                          	\
	    -Wmissing-prototypes                            	\
	    $(if $(CONFIG_KASAN),,-Wframe-larger-than=400)	\
	    -DVDO_VERSION=\"$(VDO_VERSION)\"			\
	    -DSTATIC=						\
	    -DINTERNAL                                      	\
	    -DTEST_INTERNAL                                 	\
	    -DVDO_INTERNAL					\
	    $(INCLUDES)

obj-m += kvdo.o

kvdo-objs = $(OBJECTS)
