##
# Tests for online growth of logical space (VDOSTORY-11).
#
# $Id$
##
package VDOTest::GrowLogical01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEqualNumeric
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
     blockCount    => 5000,
     # @ple Use a VDO device
     deviceType    => "lvmvdo",
     # @ple The increment to grow by
     logicalGrowth => 40 * $GB,
     # @ple Initial logical size
     logicalSize   => 5 * $GB,
     # @ple Initial physical size
     physicalSize  => 5 * $GB,
     # @ple the number of bits in the VDO slab
     slabBits      => $SLAB_BITS_TINY,
    );
##

#############################################################################
# Basic test: do growLogical while writing to the device
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  my $slice1 = $self->createSlice(blockCount => $self->{blockCount});
  my $task = Permabit::VDOTask::SliceOperation->new($slice1, "write",
                                                    direct => 1,
                                                    tag    => "basic");
  $self->getAsyncTasks()->addTask($task);

  $task->start();
  sleep(1);

  my $newLogicalSize = $self->{logicalSize} + $self->{logicalGrowth};
  $device->growLogical($newLogicalSize);
  $task->result();

  $newLogicalSize += $device->getLogicalMetadataSize();
  my $initialLogicalBlocks = int($self->{logicalSize} / $self->{blockSize});
  my $slice2 = $self->createSlice(
                                  blockCount => $self->{blockCount},
                                  offset     => $initialLogicalBlocks,
                                 );
  $slice2->write(tag => "basic", direct => 1);

  $slice1->verify();
  $slice2->verify();

  my $newLogicalBlocks = int($newLogicalSize) / $self->{blockSize};
  assertEqualNumeric($device->getVDOStats()->{"logical blocks"},
                     $newLogicalBlocks,
                     "logical blocks should reflect grow operation");
}

#############################################################################
# Make sure the volume group has an extent size of one KB so that rounding
# doesn't mess up our calculations.
##
sub propertiesMinimumGrowth {
  logicalVolumeExtentSize => $KB,
}

#############################################################################
##
sub testMinimumGrowth {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  # Check that growing by less than the block size fails.
  my $newLogicalSize = $self->{logicalSize} + $self->{blockSize};
  eval {
    $device->growLogical($newLogicalSize - $KB);
  };
  assertEvalErrorMatches(qr/reload ioctl on .* failed/);
  assertNENumeric(0, $machine->getStatus());

  # Check that growing by the block size succeeds.
  # $device->growLogical() asserts on failure.
  $device->growLogical($newLogicalSize);
}

1;
