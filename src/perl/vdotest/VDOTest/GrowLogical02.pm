##
# Tests for online growth of logical space (VDOSTORY-11).
#
# $Id$
##
package VDOTest::GrowLogical02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants;
use Permabit::GenDataFiles qw(genDataFiles);
use Permabit::Utils qw(reallySleep);
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
     deviceType    => "lvmvdo-linear",
     # @ple The increment to grow by
     logicalGrowth => 40 * $GB,
     # @ple Initial logical size
     logicalSize   => 5 * $GB,
     # @ple Initial physical size
     physicalSize  => 20 * $GB,
     # @ple the number of bits in the VDO slab
     slabBits      => $SLAB_BITS_TINY,
     # @ple use a filesystem
     useFilesystem => 1,
    );
##

#############################################################################
# Write some data through a filesystem while doing a growLogical and then
# power cycle the machine.  Check that the data is preserved.
##
sub testFilesystem {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $fs     = $self->getFileSystem();
  my $newLogicalSize = $self->{logicalSize} + $self->{logicalGrowth};

  my $slice = $self->createSlice(fs         => $fs,
                                 fileCount  => 100,
                                 totalBytes => 100 * $MB);
  my $task = Permabit::VDOTask::SliceOperation->new($slice, "write",
                                                    dedupe => 0.25,
                                                    tag    => "initial");
  $self->getAsyncTasks()->addTask($task);
  $task->start();

  reallySleep(2);
  $device->growLogical($newLogicalSize);
  $fs->resizefs();
  $task->result();

  my $fullLogicalSize = $newLogicalSize + $device->getLogicalMetadataSize();
  my $fullLogicalBlocks = int($fullLogicalSize) / $self->{blockSize};
  assertEqualNumeric($device->getVDOStats()->{"logical blocks"},
                     $fullLogicalBlocks,
                     "logical blocks should reflect grow operation");

  $fs->unmount();
  $self->rebootMachineForDevice($device);
  $fs->mount();

  # Verify that the filesystem can be used
  $slice->verify();
  genDataFiles(
               dedupe   => 0.25,
               fs       => $fs,
               numBytes => 100 * $MB,
               numFiles => 100,
              );

  assertEqualNumeric($device->getVDOStats()->{"logical blocks"},
                     $fullLogicalBlocks,
                     "logical blocks should reflect grow operation");

}

1;
