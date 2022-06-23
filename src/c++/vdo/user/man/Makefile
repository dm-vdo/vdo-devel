#
# %COPYRIGHT%
#
# %LICENSE%
#
# VDO user-space program man pages Makefile
#
# $Id$
#

PAGES= \
	vdoaudit.8               \
	vdodebugmetadata.8       \
	vdodmeventd.8            \
	vdodumpblockmap.8        \
	vdodumpconfig.8          \
	vdodumpmetadata.8        \
	vdoforcerebuild.8        \
	vdoformat.8              \
	vdolistmetadata.8        \
	vdoreadonly.8            \
	vdoregenerategeometry.8  \
	vdosetuuid.8             \
	vdostats.8

CATFILES= $(PAGES:.8=.cat)
PDFFILES= $(PAGES:.8=.pdf)
HTMLFILES=$(PAGES:.8=.html)

ROFF=groff

all:

# Generated HTML files don't look very good at the moment; omit them
# by default.
#
# Some of our build platforms don't have the PDF support.
man: $(CATFILES) # $(PDFFILES) $(HTMLFILES)

cat: $(CATFILES)
pdf: $(PDFFILES)
html: $(HTMLFILES)

# These are the equivalent of what "man" will generate and feed
# through a pager. They include traditional printer formatting like
# X^HX (overstrike) for bold text and _^HX for underlined text.
%.cat : %.8
	$(ROFF) -mandoc -w mac -Tascii $^ > $@
# PDF documentation
%.pdf : %.8
	$(ROFF) -mandoc -Tpdf $^ > $@
# HTML documentation, not very well formatted with the current tools.
%.html : %.8
	$(ROFF) -mandoc -Thtml $^ > $@

clean:
	$(RM) $(CATFILES) $(PDFFILES) $(HTMLFILES) *.tmp

# Check that we can generate output files without errors.
#
# Check that command and option lists are consistent with the
# program's help message. Option value choices (e.g., sync/async) are
# harder, and not done for now.
#
# TODO: Check consistency between per-command acceptable option lists
# and the option documentation.

checkin: man

# For comparison against the output of:
#
#   vdostats --verbose $VDODEVICE | \
#     sed -n -e '/^  /!d' -e 's/^ *//' -e 's/ *:.*$//' \
#         -e '/^bios /s/ \(discard\|flush\|fua\|read\|write\)$/.../' -e p | \
#     env LC_ALL=C sort -u
#
# (that's an actual ellipsis, to match our documentation) which we
# can't do automatically because we need an existing VDO device.
#
# Explanation of the sed commands below that process vdostats.cat:
#
# Strip out anything before the line about how stats are reported in YAML.
#
# Strip out anything that doesn't have the right indentation level or
# doesn't start with bold text (x^Hx or ESC [ 1 m) that starts with a
# letter (i.e., not "--foo"). This gets rid of the descriptive
# paragraphs mixed in with the output field descriptions.
#
# Remove formatting, then remove leading indentation.
#
# (Currently nothing at the top level of the field descriptions has a
# short enough name for the field label and field description to be on
# the same line, so we don't need to worry about that case.)
#
# Filter out the man page references ("vdo(8)" in the SEE ALSO
# section).
vdostatsmanfields.tmp: vdostats.cat
	sed -n \
	    -e '1,/YAML/d' \
	    -e '/^ \{7\}\([a-zA-Z0-9]\)\x08\1\|^ \{7\}\x1b\[1m[a-zA-Z0-9]/!d' \
	    $(SED_STRIP_GROTTY_FORMATTING) \
	    -e 's/^ *//' \
	    -e '/^[a-z]*([0-9])/d' \
	    -e p $< | $(SORT) > $@

.PHONY: all doc clean check checkin man cat pdf html
