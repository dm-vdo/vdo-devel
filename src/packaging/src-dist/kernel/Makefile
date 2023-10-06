VDO_VERSION = %%VDOVersion%%

OBJECTS = dm-vdo-target.o
OBJECTS += $(patsubst %.c,dm-vdo/%.o,$(notdir $(wildcard $(src)/dm-vdo/*.c)))

INCLUDES = -I$(src)/dm-vdo

EXTRA_CFLAGS =	-std=gnu11					\
		-fno-builtin-memset				\
		-fno-omit-frame-pointer				\
		-fno-optimize-sibling-calls			\
		-Werror						\
		$(if $(CONFIG_KASAN),,-Wframe-larger-than=400)	\
		-DVDO_VERSION=\"$(VDO_VERSION)\"		\
		-DSTATIC=					\
		$(INCLUDES)

obj-m += kvdo.o

kvdo-objs = $(OBJECTS)
