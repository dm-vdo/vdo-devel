##
# Test that requesting a VDO device with a full index succeeds.
#
# The tests that want to do use this option are all performance tests.  We just
# want to make sure that this part of the infrastructure does the right thing.
#
# $Id$
##
package VDOTest::BlockDevice03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertGTNumeric assertNumArgs);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple fill index first
     albFill    => 1,
     # @ple use a managed vdo device
     deviceType => "lvmvdo",
    );
##

########################################################################
##
sub testIndexPreFull {
  my ($self) = assertNumArgs(1, @_);
  my $stats = $self->getDevice()->getVDOStats();
  $stats->logStats("Device with full index");
  # We started with a new VDO and a new index, and it should have been filled.
  # Check that we added so many entries that we started to expire the oldest
  # ones.
  assertGTNumeric($stats->{"posts not found"}, $stats->{"entries indexed"});
}

1;
