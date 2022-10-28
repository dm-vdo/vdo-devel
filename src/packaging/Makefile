# SPDX-License-Identifier: GPL-2.0-only
#
# Packaging top level Makefile
#
# Copyright Red Hat
#

SUBDIRS = github

.PHONY: clean
clean:
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

.PHONY: cleanall
cleanall: clean

checkin:
	set -e;					\
	if [ -x "$$(command -v yamllint)" ]; then      \
		yamllint -d relaxed */*.yaml;   \
	fi
