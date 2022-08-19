# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#

TOP_DIR      = ../..
PLATFORM_DIR = ..

include $(TOP_DIR)/defines
include $(PLATFORM_DIR)/defines

vpath %.c $(PLATFORM_DIR)/uds
vpath %.c $(SRC_UDS_DIR)
vpath %.c $(SRC_UDS_DIR)/murmur
vpath %.c $(SRC_UDS_DIR)/util

# Flags for linking the shared library itself.
SHLIBFLAGS = -lm -ldl -pthread -lrt $(shell getconf LFS_LDFLAGS)

CFLAGS 	  = $(PLATFORM_CFLAGS) -I$(SRC_UDS_DIR)
LDFLAGS   = $(GLOBAL_LDFLAGS)
LDSHFLAGS = -shared -Wl,--unresolved-symbols=ignore-in-shared-libs

LIBRARIES =	libuds.a				\
		libuds.so				\
		libuds-int.so

UDS_OBJECTS =	$(notdir $(UDS_COMMON_SOURCES:%.c=%.o))	\
		blkdev.o		\
		directoryReader.o	\
		directoryUtils.o	\
		dm-bufio.o		\
		fileUtils.o		\
		logger.o		\
		memoryAlloc.o		\
		minisyslog.o		\
		requestQueue.o		\
		syscalls.o		\
		threadCondVar.o		\
		threadMutex.o		\
		threadSemaphore.o	\
		uds-threads.o

.PHONY: all
all: $(LIBRARIES)

.PHONY: clean
clean:
	rm -f *.o *.a *.s *.so lib*.so.*
	rm -rf $(DEPDIR) $(PROFDIR)

libuds.so: $(UDS_OBJECTS)
	$(CC) $(LDFLAGS) $(LDSHFLAGS) \
	$(UDS_OBJECTS) $(SHLIBFLAGS) -o $@

libuds-int.so: $(UDS_OBJECTS)
	$(CC) $(LDFLAGS) $(LDSHFLAGS) $(UDS_OBJECTS) $(SHLIBFLAGS) -o $@

libuds.a: $(UDS_OBJECTS)
	rm -f $@
	ar cr $@ $^

%.s: %.c
	$(CC) $(CFLAGS) -S $^

########################################################################
# Profiling

PROFLIBS  = $(addprefix $(PROFDIR)/, libuds.a)

profiled:	$(PROFLIBS)

$(PROFDIR)/libuds.a: $(addprefix $(PROFDIR)/,$(UDS_OBJECTS))
	rm -f $@
	ar cr $@ $^

$(PROFDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) -pg -DNDEBUG $(CFLAGS) $< -c -o $@

########################################################################
# Dependency processing

%.o: %.c
	@mkdir -p $(DEPDIR)/$(@D) $(@D)
	$(COMPILE.c) -MD -MF $(DEPDIR)/$*.d.new -MP -MT $@ \
		-MT "\$$(PROFDIR)/$@" $< -o $@
	if cmp -s $(DEPDIR)/$*.d $(DEPDIR)/$*.d.new; then \
		rm -f $(DEPDIR)/$*.d.new ; \
	else \
		mv -f $(DEPDIR)/$*.d.new $(DEPDIR)/$*.d ; \
	fi

$(DEPDIR)/%.d: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MM -MF $@ -MP -MT $*.o -MT "\$$(PROFDIR)/$*.o" $<

ifneq ($(MAKECMDGOALS),clean)
-include $(UDS_OBJECTS:%.o=$(DEPDIR)/%.d)
endif
