# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#

TOP_DIR      = ../..
TEST_DIR     = $(TOP_DIR)/src/tests
PLATFORM_DIR = ..
BUILD_DIR    = $(PLATFORM_DIR)/build

include $(TOP_DIR)/defines
include $(PLATFORM_DIR)/defines

vpath %.c $(PLATFORM_DIR)/uds
vpath %.c $(TEST_DIR)
vpath %.c $(SRC_UDS_DIR)

INCLUDES  = -I. -I$(TEST_DIR) -I$(SRC_UDS_DIR)
CFLAGS 	  = $(PLATFORM_CFLAGS) $(INCLUDES) -Wno-write-strings
LDFLAGS   = $(GLOBAL_LDFLAGS)
LDSHFLAGS = -shared
LDPRFLAGS = -ldl -lm -pthread -lrt

PROGS     = albtest dropCaches $(PERF_PROGS)
LIBRARIES = libuds-util.a

DEPLIBS = $(BUILD_DIR)/libuds-int.so
LIBS    = -L$(BUILD_DIR) -luds-int -ldl
LIBFLAGS = -pthread -lrt
PERFLIBS = $(BUILD_DIR)/libuds.a
PROFLIBS = $(BUILD_DIR)/$(PROFDIR)/libuds.a

# Tests that are not ready to run at this time
TEST_EXCLUDES =

# Any source file matching the pattern <word>_[ntx]<digit>.c is a C unit test
T_TEST_SOURCES = $(filter-out $(TEST_EXCLUDES:%=%.c), $(notdir	\
			$(wildcard ./*_t[0-9].c)		\
			$(wildcard $(TEST_DIR)/*_t[0-9].c)))
N_TEST_SOURCES = $(filter-out $(TEST_EXCLUDES:%=%.c), $(notdir	\
			$(wildcard ./*_n[0-9].c)		\
			$(wildcard $(TEST_DIR)/*_n[0-9].c)))
X_TEST_SOURCES = $(filter-out $(TEST_EXCLUDES:%=%.c), $(notdir	\
			$(wildcard ./*_x[0-9].c)		\
			$(wildcard $(TEST_DIR)/*_x[0-9].c)))
TEST_SOURCES = $(N_TEST_SOURCES) $(T_TEST_SOURCES) $(X_TEST_SOURCES)
N_TESTS	     = $(N_TEST_SOURCES:.c=.so)
T_TESTS	     = $(T_TEST_SOURCES:.c=.so)
X_TESTS	     = $(X_TEST_SOURCES:.c=.so)
TESTS        = $(T_TESTS) $(N_TESTS) $(X_TESTS)
OLDTESTS     = $(filter-out ${TESTS}, $(wildcard *_[ntx][0-9].so))

TESTLIBS     =  -Wl,-rpath,'$$ORIGIN' -L. -ludsTest		\
		-Wl,--no-whole-archive $(LIBS)
TESTDEPLIBS  = libudsTest.so $(DEPLIBS)

# Any source file matching the pattern <word>_p<digit>.c is a
# performance/profiling test
PRF_SOURCES = $(filter-out $(TEST_EXCLUDES:%=%.c), $(notdir	\
			$(wildcard ./*_p[0-9].c)		\
			$(wildcard $(TEST_DIR)/*_p[0-9].c)))
PERF_PROGS  = $(PRF_SOURCES:.c=.perftest)
PROF_PROGS  = $(PRF_SOURCES:.c=.profile)

# These profiling tests are run by the profile target
PROFILE_TESTS = MasterIndex_p1 PostBlockName_p1
PROFILE_POUTS = $(PROFILE_TESTS:%=%.pout)
PROFILE_PROFS = $(PROFILE_TESTS:%=%.prof)

ALL_COMMON =	assertions.o			\
		blockTestUtils.o		\
		cbStatus.o			\
		configUtils.o			\
		createConfigForAlbtest.o	\
		fillBufferFromSeed.o		\
		getTestIndexNames.o		\
		oldInterfaces.o			\
		processManager.o		\
		testRequests.o			\
		testUtils.o			\
		timeUtils.o

TEST_COMMON =	$(ALL_COMMON)			\
		convertToLVM.o			\
		copyDevice.o			\
		createCollidingBlock.o		\
		createRandomBlockNameInZone.o	\
		fillChapterRandomly.o		\
		getMemTotalInGB.o		\
		makeDenseConfiguration.o	\
		validateDeltaLists.o		\
		volumeUtils.o

PERF_COMMON =	$(ALL_COMMON)			\
		albtestCommon.o			\
		indexPerfCommon.o		\
		perftest.o			\
		resourceUsage.o

LOGDIR ?= $(CURDIR)

.PHONY:	all
all: oldtests $(PROGS) $(LIBRARIES) $(TESTS)

.PHONY:	clean
clean:
	rm -f $(PROGS) $(PROF_PROGS) $(PROFILE_POUTS) $(PROFILE_PROFS)
	rm -f *.a *.o *.out *.perftest *.so core*
	rm -fr $(PROFDIR) $(DEPDIR)

.PHONY: list-tests
list-tests:
	@echo @CHECKIN_TESTS = 'qw('$(T_TESTS:.so=)');'
	@echo @JENKINS_TESTS = 'qw('$(T_TESTS:.so=) $(N_TESTS:.so=)');'
	@echo @FULL_TESTS = 'qw('$(TESTS:.so=)');'
	@echo @PERF_TESTS = 'qw('$(PRF_SOURCES:.c=)');'

.PHONY:	oldtests
oldtests:
	rm -f $(OLDTESTS)

.PHONY:	profiled
profile:	$(PROFILE_PROFS)

.PHONY:	profiled
profiled: $(PROF_PROGS)

ALBTEST_OBJS = albtest.o albtestCommon.o modloader.o
albtest: $(ALBTEST_OBJS) $(TESTDEPLIBS)
	$(CC) $(LDFLAGS) $(LIBFLAGS) -Wl,-rpath,'$$ORIGIN/../build' \
		-Wl,--export-dynamic $(ALBTEST_OBJS) $(TESTLIBS) -o $@

%.perftest: %.o libPerf.a $(PERFLIBS)
	$(CC) $(LDFLAGS) $^ $(LDPRFLAGS) -o $@

%.pout:	%.profile
	./$^ >$@

%.prof:	%.pout
	gprof ./$*.profile >$@

%.profile: $(addprefix $(PROFDIR)/, %.o $(PERF_COMMON)) $(PROFLIBS)
	$(CC) -pg $(LDFLAGS) $^ $(LDPRFLAGS) -o $@

libPerf.a: $(PERF_COMMON)
	ar rcs $@ $^

libuds-util.a: configUtils.o modloader.o
	ar rcs $@ $^

%.so: %.o $(TESTDEPLIBS)
	$(CC) $(LDFLAGS) $(LDSHFLAGS) $*.o $(TESTLIBS) -o $@

libudsTest.so: $(TEST_COMMON) $(DEPLIBS)
	$(CC) $(LDFLAGS) $(LDSHFLAGS) $(TEST_COMMON) $(LIBS) -o $@

$(PROFDIR)/%.o: %.c
	@mkdir -p $(PROFDIR)
	$(CC) -pg $(CFLAGS) $< -c -o $@

########################################################################
# Dependency processing

%.o: %.c
	@mkdir -p $(DEPDIR)/$(@D) $(@D)
	$(COMPILE.c) -MD -MF $(DEPDIR)/$*.d.new -MP -MT $@ \
		-MT "\$$(PROFDIR)/$@" -o $@ $<
	if cmp -s $(DEPDIR)/$*.d $(DEPDIR)/$*.d.new; then \
		rm -f $(DEPDIR)/$*.d.new ; \
	else \
		mv -f $(DEPDIR)/$*.d.new $(DEPDIR)/$*.d ; \
	fi

$(DEPDIR)/%.d: %.c
	@mkdir -p $(DEPDIR)
	$(CC) $(CFLAGS) -MM -MF $@ -MP -MT $*.o -MT "\$$(PROFDIR)/$*.o" $<

$(DEPDIR)/%.d: %.cpp
	@mkdir -p $(DEPDIR)
	$(CXX) $(CXXFLAGS) -MM -MF $@ -MP -MT $*.o -MT "\$$(PROFDIR)/$*.o" $<

ifneq ($(MAKECMDGOALS),clean)
DEP_OBJECTS = $(sort $(TEST_COMMON) $(PERF_COMMON))
-include $(ALBTEST_OBJS:%.o=$(DEPDIR)/%.d)
-include $(DEP_OBJECTS:%.o=$(DEPDIR)/%.d)
-include $(PRF_SOURCES:%.c=$(DEPDIR)/%.d)
-include $(TEST_SOURCES:%.c=$(DEPDIR)/%.d)
endif

.SECONDARY:	$(PROFILE_POUTS)
