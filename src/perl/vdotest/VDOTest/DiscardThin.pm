##
# Test discards with thin on top of vdo to make sure
# discard passdown works.
#
# $Id$
##
package VDOTest::DiscardThin;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertLTNumeric assertNumArgs);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple set up a Thin volume on top of VDO for the test
     deviceType           => "thin-lvmvdo",
     # @ple logical (provisioned) size of the exported VDO device
     logicalSize          => "100G",
     # accept large discards so that thin will pass them down
     vdoMaxDiscardSectors => 1024,
    );
##

#############################################################################
##
sub testDiscard {
  my ($self) = assertNumArgs(1, @_);
  my $stack = $self->getStorageStack();
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  $machine->dropCaches();

  # We will use these values on non-VDO devices
  my $stats = $stack->getVDOStats();
  my $blockSize  = $stats->{"block size"};
  my $blockCount = $device->getSize() / $blockSize;

  $machine->waitForDiskSelfTests();

  my $writeCount = 1024;

  # Write some blocks
  $device->ddWrite(
		   if    => "/dev/urandom",
                   of    => $device->getSymbolicPath(),
                   count => $writeCount,
                   bs    => $blockSize,
		   oflag => "direct",
                  );

  # See how many discards VDO has, before we call genDiscard
  # on the thin device.
  $stats = $stack->getVDOStats();
  my $discards = $stats->{"bios in discard"};

  # Discard the whole of the thin volume. This should send discards
  # to VDO.
  $machine->genDiscard(
                       of    => $device->getSymbolicPath(),
                       count => $blockCount,
                       bs    => $blockSize,
                      );

  # We will get enough discards to cover the amount of writes above.
  # However, we can't know for sure how many discard bios VDO will get since
  # the discard splitting between kernel versions is different. So just
  # look for any discards to show up. It will prove discards are sent
  # to VDO from Thin.
  $stats = $stack->getVDOStats();
  assertLTNumeric($discards,
		  $stats->{"bios in discard"},
		  "no discards sent to VDO");
}

1;
