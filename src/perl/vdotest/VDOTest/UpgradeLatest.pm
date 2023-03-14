##
# Verify that VDO can be upgraded from last branch.
#
# $Id$
##
package VDOTest::UpgradeLatest;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertNumArgs
);
use Permabit::Constants;

use base qw(VDOTest::UpgradeBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple The intermediate version scenarios to go through
   intermediateScenarios => [],
   # @ple VDO physical size
   physicalSize          => 50 * $GB,
   # @ple The scenario to start with
   initialScenario       => "X86_RHEL9_8.2.1-current",
   # @ple VDO slab bit count
   slabBits              => $SLAB_BITS_TINY,
  );
##

#############################################################################
# Run the basic upgrade test from UpgradeBase.
##
sub testUpgradeFromLatest {
  my ($self) = assertNumArgs(1, @_);
  $self->_runTest();
}

1;
