#
# $Id$
#

ROOT    := ..
DOCDIRS := VDOTest

TEST_ARGS := --log
ifeq ($(SAVELOGS),1)
  TEST_ARGS += --xmlOutput=1 --quiet=1
endif
ifdef LOGDIR
  TEST_ARGS += --logDir=$(LOGDIR) --saveServerLogDir=$(LOGDIR)
endif

include $(ROOT)/Makefile.common

ifeq ($(USER),$(filter $(USER),bunsen continuous))
  OPTIONAL_RSVP_HOST = PRSVP_HOST=jenkins
endif

JENKINS_CLIENT_CLASS = --clientClass=FARM\\,RHEL10
JENKINS_TEST_SUITES = checkin platform

# This is Make's form of a function -- it gets expanded every time it's used
VDOTESTS_INVOCATION = $(OPTIONAL_RSVP_HOST) ./vdotests.pl $(TEST_ARGS) \
                      $(TEST_EXTRA_ARGS) $(TEST_PARALLEL_ARGUMENT) \
		      $(TEST_SUITES)

ifeq ($(OSS_VDOBUILD), 1)
  PLATFORM_TESTS = openPlatformTests
else
  PLATFORM_TESTS = platformTests
endif

ifeq ($(DMLINUX), 1)
  TEST_ARGS += --useDmLinuxModule
  JENKINS_TEST_SUITES = checkin
endif

.PHONY: jenkins
jenkins: TEST_EXTRA_ARGS=$(JENKINS_CLIENT_CLASS)
jenkins: $(JENKINS_TEST_SUITES)

.PHONY: checkin
checkin: vdotests

.PHONY:	vdotests
vdotests: TEST_PARALLEL_ARGUMENT = --scale
vdotests: TEST_SUITES = checkin
vdotests:
	$(VDOTESTS_INVOCATION)

.PHONY: platform
# For maximum parallelism the number of threads should be >= the
# number of platforms being tested.
platform: TEST_PARALLEL_ARGUMENT = --threads=5
platform: TEST_SUITES = $(PLATFORM_TESTS)
platform:
	$(VDOTESTS_INVOCATION)

.PHONY: test-vdo-distribution
test-vdo-distribution: TEST_EXTRA_ARGS=$(JENKINS_CLIENT_CLASS)
test-vdo-distribution: TEST_PARALLEL_ARGUMENT = --scale
test-vdo-distribution: TEST_SUITES = distributionTests
test-vdo-distribution:
	$(VDOTESTS_INVOCATION)

.PHONY: cleanlogs
cleanlogs::
	-chmod -R u+w VDOTest\:\:*
	rm -rf VDOTest\:\:*


