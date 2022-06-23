##
# Run the MurmurHash3 performance test.
#
# $Id$
##
package VDOTest::MurmurHash3Perf;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Permabit::Assertions qw(assertNumArgs);

use base qw(VDOTest::UnitTestBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple use a local machine
     clientClass => "ALBIREO-PMI",
     # @ple The glob patterns for selecting tests
     globPattern => "MurmurHash3_p1.perftest",
    );
##

########################################################################
# @inherit
##
sub listSharedFiles {
  my ($self) = assertNumArgs(1, @_);
  return ($self->SUPER::listSharedFiles(),
          "src/c++/vdo/tests/MurmurHash3_p1.perftest",
         );
}

1;
