# Makefile for auto-generating vdo statistics structures, generators, and
# parsers.
#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$
#

SRC_DIR = ../..
include $(SRC_DIR)/defines

VDO_DIR          = $(C_DIR)/vdo
PERL_INSTALL_DIR = $(PERL_DIR)/Permabit/Statistics

# Names and install locations of generated files

RELEASE_VERSIONS_H           = release-versions.h
RELEASE_VERSIONS_H_INSTALLED = $(VDO_DIR)/base/$(RELEASE_VERSIONS_H)
STATS_H                      = statistics.h
STATS_H_INSTALLED            = $(VDO_DIR)/base/$(STATS_H)
MSGSTATS_C                   = message-stats.c
MSGSTATS_C_INSTALLED         = $(VDO_DIR)/base/$(MSGSTATS_C)
MSGSTATS_RDR_C               = messageStatsReader.c
MSGSTATS_RDR_C_INSTALLED     = $(VDO_DIR)/user/$(MSGSTATS_RDR_C)
SYSSTATS_C                   = pool-sysfs-stats.c
SYSSTATS_C_INSTALLED         = $(VDO_DIR)/base/$(SYSSTATS_C)
VDOSTATS_C                   = vdoStatsWriter.c
VDOSTATS_C_INSTALLED         = $(VDO_DIR)/user/$(VDOSTATS_C)
C_FILES_INSTALLED            = $(RELEASE_VERSIONS_H_INSTALLED) \
                               $(STATS_H_INSTALLED)            \
                               $(MSGSTATS_C_INSTALLED)         \
                               $(MSGSTATS_RDR_C_INSTALLED)     \
                               $(SYSSTATS_C_INSTALLED)         \
                               $(VDOSTATS_C_INSTALLED)

PERL_DEFINITIONS           = Definitions.pm
PERL_DEFINITIONS_INSTALLED = $(PERL_INSTALL_DIR)/$(PERL_DEFINITIONS)
PERL_FILES_INSTALLED       = $(PERL_DEFINITIONS_INSTALLED)
PERL_FILES                 = $(notdir $PERL_FILES_INSTALLED)

INSTALLED_FILES = $(C_FILES_INSTALLED)      \
                  $(PERL_FILES_INSTALLED)

INTERMEDIATE_FILES = $(notdir $(INSTALLED_FILES))

.NOTPARALLEL:

.PHONY: all
all: $(INSTALLED_FILES)

$(RELEASE_VERSIONS_H_INSTALLED): $(RELEASE_VERSIONS_H)
	cp $(RELEASE_VERSIONS_H) $(RELEASE_VERSIONS_H_INSTALLED)

$(STATS_H_INSTALLED): $(STATS_H)
	cp $(STATS_H) $(STATS_H_INSTALLED)

$(MSGSTATS_C_INSTALLED): $(MSGSTATS_C)
	cp $(MSGSTATS_C) $(MSGSTATS_C_INSTALLED)

$(MSGSTATS_RDR_C_INSTALLED): $(MSGSTATS_RDR_C)
	cp $(MSGSTATS_RDR_C) $(MSGSTATS_RDR_C_INSTALLED)

$(SYSSTATS_C_INSTALLED): $(SYSSTATS_C)
	cp $(SYSSTATS_C) $(SYSSTATS_C_INSTALLED)

$(VDOSTATS_C_INSTALLED): $(VDOSTATS_C) $(VDOSTATS_RDR_C_INSTALLED)
	cp $(VDOSTATS_C) $(VDOSTATS_C_INSTALLED)

$(PERL_DEFINITIONS_INSTALLED): $(PERL_DEFINITIONS)
	cp $(PERL_DEFINITIONS) $(PERL_DEFINITIONS_INSTALLED)

$(PERL_DEFINITIONS): $(VDO_STATS_DEPS)
	$(STATS_GENERATOR) --languages=Perl $(VDO_STATS_FILES)

.PHONY: clean cleanall
clean cleanall:
	rm -f $(INTERMEDIATE_FILES)

%.h: $(VDO_STATS_DEPS)
	$(STATS_GENERATOR) --languages=C $(VDO_STATS_FILES)

$(MSGSTATS_C): $(VDO_STATS_DEPS)
	$(STATS_GENERATOR) --languages=CMessage $(VDO_STATS_FILES)

$(MSGSTATS_RDR_C): $(VDO_STATS_DEPS)
	$(STATS_GENERATOR) --languages=CMessageReader $(VDO_STATS_FILES)

$(SYSSTATS_C): $(VDO_STATS_DEPS)
	$(STATS_GENERATOR) --languages=CSysfs $(VDO_STATS_FILES)

$(VDOSTATS_C): $(VDO_STATS_DEPS)
	$(STATS_GENERATOR) --languages=CVDOStats $(VDO_STATS_FILES)
