#!/usr/bin/perl

##
# Script to run tests derived from Permabit::Testcase
#
# $Id$
##

use strict;
use warnings FATAL => qw(all);
use Cwd qw(cwd);
use English qw(-no_match_vars);
use FindBin;

# Setup DEFAULT_TOPDIR.  Use BEGIN because we need to compute this value
# before the following "use lib" statements are parsed.
our $DEFAULT_TOPDIR;
BEGIN {
  $DEFAULT_TOPDIR = $FindBin::Bin;
  $DEFAULT_TOPDIR =~ s%^(.*)/(src|main)/(tools|perl)/.*?$%$1%;
}

# Use the current real value of lastrun so that things don't change if the
# symlink gets re-pointed.
use lib (defined($ENV{PERMABIT_PERL_PATH}))
         ? split('\s*,\s*', $ENV{PERMABIT_PERL_PATH})
         : "${FindBin::RealBin}/../commonlib";

# This may be invoked via a symlink, and we want to use libraries
# relative to the symlink, so use a path from $DEFAULT_TOPDIR.
use lib "$DEFAULT_TOPDIR/src/perl/lib";
use lib cwd();

use Permabit::TestRunner;

# Files which should be copied to $nfsShareDir for the tests to use.
our $SOURCE_FILES = undef;

exit(Permabit::TestRunner::main());

1;
