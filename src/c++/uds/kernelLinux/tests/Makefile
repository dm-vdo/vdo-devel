# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright Red Hat
#
TOP_DIR      = ../..
PLATFORM_DIR = ..
TEST_DIR     = $(TOP_DIR)/src/tests
BUILD_DIR    = $(PLATFORM_DIR)/build

include $(TOP_DIR)/defines
include $(PLATFORM_DIR)/defines

UDS_DIR = $(basename $(UDS_PACKAGE))
UDS_TGZ = $(BUILD_DIR)/$(UDS_PACKAGE)

ZUB_NAME    = zubenelgenubi
ZUB_VERSION = 1
ZUB_MODULE  = $(ZUB_NAME)-$(ZUB_VERSION)
ZUB_PACKAGE = $(ZUB_MODULE).tgz
ZUB_DIR     = $(ZUB_MODULE)

TEST_EXCLUDES =	$(TEST_DIR)/VolumeIndex_p2.c $(TEST_DIR)/VolumeIndex_n2.c 

TEST_SOURCES = $(filter-out $(TEST_EXCLUDES),			\
			$(wildcard ./[A-Z]*_[nptx][1-9].c)	\
			$(wildcard $(TEST_DIR)/[A-Z]*_[nptx][1-9].c))
ALL_TESTS = $(sort $(basename $(notdir $(TEST_SOURCES))))

ALLTESTS_COMMAND = $(TEST_DIR)/alltests.py AllTests_t1 "$(ALL_TESTS)"

.PHONY:	all
all:	$(ZUB_PACKAGE)

.PHONY:	clean
clean:
	rm -fr $(UDS_DIR) $(ZUB_DIR) *.tgz

###############################################################################
# Build rules for the test DKMS tarball object

ZUB_SUBDIR  = $(ZUB_DIR)/$(ZUB_NAME)
ZUB_DKMS_OBJECTS =	AllTests_t1.o				\
			$(ALL_TESTS:%=%.o)			\
			$(notdir $(ZUB_DKMS_SOURCES:%.c=%.o))

ZUB_DKMS_SOURCES =	albtest.c		\
			copyDevice.c		\
			getMemTotalInGB.c	\
			getTestIndexNames.c	\
			resourceUsage.c		\
	$(addprefix $(TEST_DIR)/,	albtestCommon.c			\
					assertions.c			\
					blockTestUtils.c		\
					cbStatus.c			\
					configUtils.c			\
					createCollidingBlock.c		\
					createConfigForAlbtest.c	\
					createRandomBlockNameInZone.c	\
					fillBufferFromSeed.c		\
					fillChapterRandomly.c		\
					indexPerfCommon.c		\
					makeDenseConfiguration.c	\
					oldInterfaces.c			\
					testRequests.c			\
					validateDeltaLists.c		\
					volumeUtils.c)


ZUB_DKMS_HEADERS = $(wildcard $(TEST_DIR)/*.h) $(wildcard ./*.h)

# These are extra EXTRA_CFLAGS for the zubenelgenubi module build.
ZUB_EXTRA = 	$(ZUB_CFLAGS) -DTEST_INTERNAL		\
		-UEXTERNAL_STATIC			\
		-DEXTERNAL_STATIC=			\
		-Wno-declaration-after-statement

.DELETE_ON_ERROR:
$(ZUB_PACKAGE): $(UDS_TGZ) $(ZUB_DKMS_SOURCES) $(ZUB_DKMS_HEADERS) \
		$(TEST_SOURCES)
	rm -fr $(UDS_DIR) $(ZUB_DIR)
	tar xvfz $(UDS_TGZ)
	echo EXTRA_CFLAGS += $(ZUB_EXTRA) >>$(UDS_DIR)/$(UDS_NAME)/Makefile
	mv $(UDS_DIR) $(ZUB_DIR)
	mkdir -p $(ZUB_SUBDIR)
	cp -p $(ZUB_DKMS_SOURCES) $(ZUB_SUBDIR)
	cp -p $(ZUB_DKMS_HEADERS) $(ZUB_SUBDIR)
	$(ALLTESTS_COMMAND) >$(ZUB_SUBDIR)/AllTests_t1.c
	for T in $(TEST_SOURCES); do B=`basename $$T .c`; \
		sed -e s/initializeModule/initializeModule_$$B/g \
		<$$T >$(ZUB_SUBDIR)/$$B.c; done
	$(call DKMS_MAKEFILE, $(ZUB_NAME), $(ZUB_DKMS_OBJECTS), \
		$(ZUB_EXTRA) -I'$$(src)/../uds' -I'$$(src)/../tests') \
		>$(ZUB_SUBDIR)/Makefile
	echo OBJECT_FILES_NON_STANDARD = y >>$(ZUB_SUBDIR)/Makefile
	echo obj-m += $(ZUB_NAME)/ >>$(ZUB_DIR)/Makefile
	$(call DKMS_CONF, $(ZUB_NAME), $(ZUB_VERSION), $(UDS_NAME), \
		$(ZUB_NAME)) >$(ZUB_DIR)/dkms.conf
	tar cvfz $@ $(ZUB_DIR)
	$(MAKE) -C $(KERNEL_SOURCE_DIR) M=$(abspath $(ZUB_DIR)) modules \
		$(if $(LLVM), $(LLVM_ARGS),)
