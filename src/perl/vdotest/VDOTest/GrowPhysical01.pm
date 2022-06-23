##
# Tests for online growth of physical space (VDOSTORY-10).
#
# $Id$
##
package VDOTest::GrowPhysical01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEvalErrorMatches
  assertNENumeric
  assertNumArgs
);
use Permabit::Constants;
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks to write
     blockCount   => 5000,
     # @ple Use a VDO device
     deviceType   => "lvmvdo",
     # @ple Initial physical size in bytes
     physicalSize => 5 * $GB,
     # @ple the number of bits in the VDO slab
     slabBits     => $SLAB_BITS_TINY,
    );
##

#############################################################################
# Resize the physical device while writing to the VDO device
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  my $slice = $self->createSlice(blockCount => $self->{blockCount});
  my $task = Permabit::VDOTask::SliceOperation->new($slice, "write",
                                                    direct => 1,
                                                    tag    => "basic");
  $self->getAsyncTasks()->addTask($task);

  $task->start();
  sleep(1);
  $device->growPhysical(20 * $GB);
  $task->result();

  $slice->verify();
}

#############################################################################
# Attempt to grow physical with no change in the physical size [VDO-4199].
# This test will not currently work with unmanaged vdo devices.
##
sub testNoGrowth {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  # LVM allows a volume to be created with a size that isn't aligned to an
  # extent, but lvresize requires aligned. As a result, the first attempt to
  # grow by zero can still actually grow the physical volume, so throw away
  # the first one.
  eval {
    $device->growPhysical($self->{physicalSize});
  };
  eval {
    $device->growPhysical($self->{physicalSize});
  };
  if ($device->isa("Permabit::BlockDevice::VDO::LVMManaged")) {
    # lvresize apparently thinks zero is a negative number
    assertEvalErrorMatches(qr/Cannot reduce VDO pool data volume/)
  } else {
    assertEvalErrorMatches(qr/New size .* matches existing size .*/);
  }
  assertNENumeric(0, $machine->getStatus());
}

#############################################################################
# Resize the physical device while VDO is not running [VDO-4220]
##
sub testOffline {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  $device->stop();
  $device->resizeStorageDevice(20 * $GB);
  $device->start();

  $device->growPhysical(20 * $GB);
}

#############################################################################
# Make sure the volume group has an extent size of one block so that rounding
# doesn't mess up our calculations.
##
sub propertiesTooSmall {
  logicalVolumeExtentSize => 4 * $KB,
}

#############################################################################
# Attempt to grow physical with too small an amount.
##
sub testTooSmall {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  eval {
    $device->growPhysical($self->{physicalSize} +
                          $self->{logicalVolumeExtentSize});
  };
  assertEvalErrorMatches(qr/reload ioctl on .* failed/);
  assertNENumeric(0, $machine->getStatus());
}

1;
