##
# Verify that the VDO compression setting specified at create time is
# maintained through an upgrade.
#
# $Id$
##
package VDOTest::UpgradeCompressDedupe;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertFalse
  assertNumArgs
  assertTrue
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
   # @ple The scenario to start with
   initialScenario      => { version => "8.1.0-current" },
   # @ple VDO slab bit count
   slabBits             => $SLAB_BITS_TINY,
  );
##

#############################################################################
# Run the basic upgrade test from UpgradeBase. Instead of doing the usual
# verify step, just check whether compression is what we expect.
##
sub propertiesUpgradeNoCompress {
  return ( enableCompression => 0 );
}

sub testUpgradeNoCompress {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  $self->_runTest(1);

  my $compression = $device->isVDOCompressionEnabled();
  assertFalse($compression, "Compression should be off");
}

#############################################################################
# Run the basic upgrade test from UpgradeBase. Instead of doing the usual
# verify step, just check whether compression is what we expect.
##
sub propertiesUpgradeWithCompress {
  return ( enableCompression => 1 );
}

sub testUpgradeWithCompress {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  $self->_runTest(1);

  my $compression = $device->isVDOCompressionEnabled();
  assertTrue($compression, "Compression should be on");
}

#############################################################################
# Run the basic upgrade test from UpgradeBase. Instead of doing the usual
# verify step, just check whether deduplication is what we expect.
##
sub propertiesUpgradeNoDedupe {
  return (
	  disableAlbireo      => 1,
	  enableDeduplication => 0,
	 );
}

sub testUpgradeNoDedupe {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  $self->_runTest(1);

  my $deduplcation = $device->getVDODedupeEnabled();
  assertFalse($deduplcation, "Deduplication should be off");
}

#############################################################################
# Run the basic upgrade test from UpgradeBase. Instead of doing the usual
# verify step, just check whether deduplication is what we expect.
##
sub propertiesUpgradeWithDedupe {
  return ( enableDeduplication => 1 );
}

sub testUpgradeWithDedupe {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  $self->_runTest(1);

  my $deduplcation = $device->getVDODedupeEnabled();
  assertTrue($deduplcation, "Deduplication should be on");
}

1;
