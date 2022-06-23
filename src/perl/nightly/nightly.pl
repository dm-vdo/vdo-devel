#!/usr/bin/perl -w

##
# Build and run tests on Permabit source tree.
#
# $Id$
##

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Cwd qw(abs_path);
use FindBin;
use lib (
  $FindBin::RealBin,
  "${FindBin::RealBin}/../lib",
  ($ENV{PERMABIT_PERL_PATH}
   ? split('\s*,\s*', $ENV{PERMABIT_PERL_PATH})
   : (abs_path("/permabit/build/common/lastrun/perl") . '/lib',
      abs_path("/permabit/build/perl/lastrun") . '/nightly',)
  ),
);

use Nightly;
exit(Nightly::main());

