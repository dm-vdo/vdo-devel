#
# $Id$
#

ROOT    := ..

TEST_ARGS := --log --threads=8
ifeq ($(SAVELOGS),1)
  TEST_ARGS += --log=1 --xmlOutput=1 --quiet=1
endif
ifdef LOGDIR
  TEST_ARGS += --logDir=$(LOGDIR) --saveServerLogDir=$(LOGDIR)
endif

ifeq ($(DMLINUX), 1)
  TEST_ARGS += --useDmLinuxModule
endif

ifeq ($(USER),continuous)
  RSVP_HOST = PRSVP_HOST=jenkins
endif

.PHONY: checkin
checkin:
	./dmtests.pl $(TEST_ARGS) $@

.PHONY: jenkins
jenkins:
	$(RSVP_HOST) ./dmtests.pl $(TEST_ARGS) \
	  --scale --clientClass=FARM\\,FEDORA41 $@

.PHONY:	dmtests
dmtests: checkin

.PHONY: cleanlogs
cleanlogs::
	rm -rf DMTest\:\:*
