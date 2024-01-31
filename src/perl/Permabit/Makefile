# $Id$

ROOT      = ..
DOCDIRS   =	BlockDevice	\
		CommandString	\
		FileSystem	\
		MegaRaid	\
		VDOTask		\
		testcases
MANPREFIX = Permabit::

TEST_ARGS := --log
ifeq ($(SAVELOGS),1)
  TEST_ARGS += --log=1 --xmlOutput=1 --quiet=1
endif
ifdef LOGDIR
  TEST_ARGS += --logDir=$(LOGDIR) --saveServerLogDir=$(LOGDIR)
endif

include $(ROOT)/Makefile.common

cleanlogs::
	rm -rf testcases\:\:*

runtests:
	./$@.pl $(TEST_ARGS) --scale checkin
