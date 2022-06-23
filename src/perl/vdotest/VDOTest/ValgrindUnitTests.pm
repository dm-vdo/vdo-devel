##
# Run unit tests with valgrind.
#
# $Id$
##
package VDOTest::ValgrindUnitTests;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use base qw(VDOTest::UnitTestBase);

our %PROPERTIES
  = (
     # @ple The glob patterns for selecting tests
     globPattern => "*_t[0-9].so",
     # @ple run the scanner binary with valgrind
     valgrind    => 1,
    );

1;
