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
   # @ple The intermediate versions to go through
   intermediateVersions => [],
   # @ple VDO physical size
   physicalSize         => 50 * $GB,
   # @ple The version to start with
   setupVersion         => "8.1.0-current",
   # @ple VDO slab bit count
   slabBits             => $SLAB_BITS_TINY,
  );
##

#############################################################################
# Run the basic upgrade test from UpgradeBase.
##
sub testUpgradeFromLatest {
  my ($self) = assertNumArgs(1, @_);
  $self->_runUpgradeTest();
}

1;
