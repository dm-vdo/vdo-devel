# vdo tests Makefile
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$

# The rpath stuff below depends on C_DIR being a relative path. If
# you build with an absolute C_DIR, you may have to use
# LD_LIBRARY_PATH to find all of the shared libraries at run time.
# Since C_DIR is derived from SRC_DIR, SRC_DIR must be relative as well.
SRC_DIR = ../../..
include ../defines
UDS_TESTS_DIR = $(UDS_TOP)/userLinux/tests

VDOTEST = ./vdotest

INCLUDES = $(UDS_INCLUDES) -I$(UDS_TESTS_DIR) \
           -I$(VDO_USER_DIR) -I$(VDO_BASE_DIR) -I$(VDO_KERNEL_DIR) \
           -isystem $(VDO_FAKE_DIR)

CFLAGS 	  = -fPIC $(GLOBAL_CFLAGS) $(INCLUDES) -Wno-write-strings \
            -DINTERNAL -DTEST_INTERNAL

LDFLAGS   = $(GLOBAL_LDFLAGS)
LDSHFLAGS = -shared
LDPRFLAGS = -ldl -lm -lz -pthread

# N.B. We need explicit rules below to make sure each one of these is up to
# date; we can't use one rule for the VDO_DEPLIBS variable so long as there are
# objects shared between the libraries.
VDO_DEPLIBS = $(VDO_USER_DIR)/libuser.a    \
              $(VDO_BASE_DIR)/libvdo.a     \
              $(VDO_BASE_DIR)/libvdouser.a \
              $(VDO_FAKE_DIR)/libvdofake.a

UDS_DEPLIBS = $(UDS_TESTS_DIR)/libuds-util.a $(UDS_USER_BUILD_DIR)/libuds.a
DEPLIBS     = $(VDO_DEPLIBS) $(UDS_DEPLIBS)
LIBS        = $(DEPLIBS) -ldl -luuid
LIBFLAGS    = -pthread -lrt -lm

$(UDS_DEPLIBS): $(UDS_USER_FILES)
	$(MAKE) -C $(UDS_USER_DIR)

# Tests that are not ready to run at this time
TEST_EXCLUDES =

# Any source file matching the pattern <word>_[tvx]<digits>.c is a C unit test
TEST_SOURCES     = $(filter-out ${TEST_EXCLUDES}, $(wildcard *_[tvx][0-9]*.c))
PERF_TESTS       = $(filter-out ${TEST_EXCLUDES}, $(wildcard *_p[0-9].c))
VALGRIND_SOURCES = $(filter-out ${TEST_EXCLUDES}, $(wildcard *_[t][0-9]*.c))
TESTS	         = $(TEST_SOURCES:.c=.so)
PERF_PROGS       = $(PERF_TESTS:.c=.perftest)
OLDTESTS         = $(filter-out ${TESTS}, $(wildcard *_[tvx][0-9]*.so))
EXTRAS	         = __vdotest__init.so

TESTLIBS     = -Wl,-rpath,'$$ORIGIN' -L. -lvdoTest $(LIBS)
TESTDEPLIBS  = libvdoTest.so $(DEPLIBS)

TEST_COMMON  = adminUtils.o              \
               asyncLayer.o              \
               asyncVIO.o                \
               blockAllocatorUtils.o     \
               blockMapUtils.o           \
               callbackWrappingUtils.o   \
               completionUtils.o         \
               dataBlocks.o              \
               dump.o                    \
               dm-kcopyd.o               \
               intIntMap.o               \
               ioRequest.o               \
               journalWritingUtils.o     \
               latchedCloseUtils.o       \
               latchUtils.o              \
	       lz4.o			 \
	       mempool.o		 \
               mutexUtils.o              \
               packerUtils.o             \
               processManager.o          \
               ramLayer.o                \
               recoveryModeUtils.o       \
               slabSummaryUtils.o        \
               sparseLayer.o             \
               testBIO.o                 \
               testDM.o                  \
               testParameters.o          \
               testTimer.o               \
               testUtils.o               \
               vdoTestBase.o             \
               workQueue.o

STRUCT_HEADERS = repairCompletion.h

GENERATED_FILES = $(STRUCT_HEADERS) dump.c

LOGDIR ?= $(CURDIR)

.PHONY: all
all: $(PROGS) $(TESTS) $(PERF_PROGS) $(EXTRAS) $(VDOTEST)
	for T in $(OLDTESTS); do rm $$T; done

# This can cause competing command runs under parallel builds.
#$(VDO_DEPLIBS): %: FORCE
#	$(MAKE) -C $(@D) $(@F)
# Instead:
$(VDO_USER_DIR)/libuser.a: FORCE
	$(MAKE) -C $(VDO_USER_DIR) libuser.a
$(VDO_FAKE_DIR)/libvdofake.a: FORCE
	$(MAKE) -C $(VDO_FAKE_DIR) libvdofake.a
# These share some object files, so we must build them in one make invocation
# or force serialization.
$(VDO_BASE_DIR)/libvdo.a $(VDO_BASE_DIR)/libvdouser.a:
	$(MAKE) -C $(VDO_BASE_DIR) libvdo.a libvdouser.a

.PHONY: clean
clean:
	rm -f $(PROGS) $(PERF_PROGS) $(VDOTEST) *.so *.o vdo-checkin.xml core*
	rm -fr $(DEPDIR) tmpdir $(GENERATED_FILES)

checkin: all
ifeq ($(SAVELOGS),1)
	$(VDOTEST) --xml=$(LOGDIR)/vdo-checkin.xml
else
	$(VDOTEST)
endif

.PHONY: valgrind
valgrind:
	set -e; \
	for i in $(VALGRIND_SOURCES:.c=); do \
		valgrind --leak-check=full -q $(VDOTEST) --no-unload --no-fork $$i; \
	done;

lz4.o: CFLAGS:= $(filter-out -Wcast-qual,$(CFLAGS))

%.perftest: %.o $(TESTDEPLIBS)
	$(CC) $(LDFLAGS) $^ $(TESTLIBS) -o $@

%.so: %.o $(TESTDEPLIBS)
	$(CC) $(LDFLAGS) $(LDSHFLAGS) $*.o $(TESTLIBS) -o $@

libvdoTest.so: $(TEST_COMMON) $(DEPLIBS)
	$(CC) $(LDFLAGS) $(LDSHFLAGS) $(TEST_COMMON) $(LDPRFLAGS) $(LIBS) -o $@

vdotest: vdotest.o $(TESTDEPLIBS)
	$(CC) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/../uds' \
		vdotest.o $(TESTLIBS) $(LIBFLAGS) -o $@

define struct_header =
  $(1): $(2)
	echo '#ifndef GUARD_$(3)_GUARD' > $(1)
	echo '#define GUARD_$(3)_GUARD' >> $(1)
	sed '/^struct $(3)/Q' $(2) >> $(1)
	sed -n '/^struct $(3)/, /^};/p;' $(2) \
	  | sed -n '/./, /^};/p;/^};/q;' >> $(1)
	echo '#endif /* not GUARD_$(3)_GUARD */' >> $1
endef

$(eval $(call struct_header,repairCompletion.h,$(VDO_BASE_DIR)/repair.c,repair_completion))

dump.c: $(VDO_BASE_DIR)/dump.c dump.h Makefile
	sed -e 's/%llu/%lu/g' < $< > $@

.PHONY: FORCE
FORCE:

########################################################################
# Dependency processing

%.o: %.c
	$(COMPILE.c) -MD -MF $(DEPDIR)/$*.d.new -MP -MT $@ -o $@ $<
	if cmp -s $(DEPDIR)/$*.d $(DEPDIR)/$*.d.new ;		 \
	then							 \
		$(RM) $(DEPDIR)/$*.d.new ;			 \
	else							 \
		$(MV) $(DEPDIR)/$*.d.new $(DEPDIR)/$*.d ;	 \
	fi

$(DEPDIR)/%.d: %.c $(FAKE_INCLUDES) $(STRUCT_HEADERS)
	@mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -M -MF $@ -MP -MT $*.o $<

ifneq ($(MAKECMDGOALS),clean)
DEPSOURCES = $(wildcard *.c)
-include $(DEPSOURCES:%.c=$(DEPDIR)/%.d)
endif
