VDO_VERSION = %%VDOVersion%%

SOURCES  = $(notdir $(wildcard $(src)/*.c))
OBJECTS = $(SOURCES:%.c=%.o)
INCLUDES = -I$(src)

ccflags-y = -std=gnu11						\
	    -fno-builtin-memset					\
	    -Werror						\
	    $(if $(CONFIG_KASAN),,-Wframe-larger-than=400)	\
	    -DVDO_VERSION=\"$(VDO_VERSION)\"			\
	    $(INCLUDES)

obj-m += kvdo.o

kvdo-objs = $(OBJECTS)
