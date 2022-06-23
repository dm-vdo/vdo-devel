##
# Test the VDO physical growth application
#
# $Id$
##
package VDOTest::GrowPhysical02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertDefined assertNumArgs);
use Permabit::Constants;
use Permabit::Utils qw(sizeToLvmText);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $DATA_SET_COUNT   = 6;
my $RESIZE_INCREMENT = $GB;

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple use a local machine with sync storage
     clientClass    => "VDO-PMI",
     # @ple Use a VDO device
     deviceType     => "lvmvdo",
     # @ple disable albireo since this test does not care about dedupe
     disableAlbireo => 1,
     # @ple logical (provisioned) size of the VDO device
     logicalSize    => ($DATA_SET_COUNT + 1) * $RESIZE_INCREMENT,
     # @ple starting physical size of the exported VDO partition
     physicalSize   => $GB,
     # @ple how big the slab gets to be
     slabBits       => $SLAB_BITS_TINY,
     # @ple the number of blocks written
     _used          => 0,
     # @ple the current logical volume size
     _vdoSize       => $GB,
    );
##

#############################################################################
# Generate all the datasets used for this test.
##
sub _initializeDatasets {
  my ($self, $count) = assertNumArgs(2, @_);
  my %datasets;
  my $blocksPerIncrement = $RESIZE_INCREMENT / $self->{blockSize};
  for (my $set = 0; $set < $count; $set++) {
    my %dataset;
    $dataset{tag}    = "$set";
    # This assumes no resize results in more added free blocks than the resize
    # increment. This is generally an unsafe assumption, but because we
    # are growing by a whole number of slabs, it is safe in this case.
    $dataset{offset} = $set * $blocksPerIncrement;
    # Use the last block in the dataset for the single block write.
    $dataset{singleBlockSlice}
      = $self->createSlice(
                           blockCount => 1,
                           blockSize  => $self->{blockSize},
                           offset     => ($set + 1) * $blocksPerIncrement - 1,
                          );
    $dataset{slice} = undef; # Created later when we write data.
    $datasets{$set} = \%dataset;
   }

  return \%datasets;
}

#############################################################################
# Fill the VDO to capacity.
##
sub _fillPhysicalSpace {
  my ($self, $device, $dataset) = assertNumArgs(3, @_);
  $log->info("Filling the logical volume with data");

  # Determine the number of blocks required to fill all remaining space.
  my $vdoStats      = $device->getVDOStats();
  my $totalBlocks   = $vdoStats->{"physical blocks"};
  my $usableBlocks  = $totalBlocks - $vdoStats->{"overhead blocks used"};
  my $blocksToWrite = $usableBlocks - $self->{_used};

  # Fill the VDO with data, doing that by large increments to speed up writing.
  # Write as many blocks as we can in large batches.
  my $logicalStart  = $dataset->{offset};
  $dataset->{slice} =  $self->createSlice(
                                          blockCount => $blocksToWrite,
                                          blockSize  => $self->{blockSize},
                                          offset     => $logicalStart,
                                         );
  $dataset->{slice}->write(tag    => "write" . $dataset->{tag},
                           direct => 1);

  # Verify that another block write will fail since there is no more space.
  $self->createSlice(
                     blockCount => 1,
                     blockSize  => $self->{blockSize},
                     offset     => $logicalStart + $blocksToWrite,
                    )->writeENOSPC(
                                   tag     => "nospace",
                                   direct  => 1
                                  );
  $self->{_used} += $blocksToWrite;
}

#############################################################################
# Write one block.
##
sub _writeOneBlock {
  my ($self, $device, $dataset) = assertNumArgs(3, @_);
  $log->info("Writing one more block to prove that more space exists");
  $dataset->{singleBlockSlice}->write(tag    => "one" . $dataset->{tag},
                                      direct => 1,
                                      fsync  => 1,
                                     );
  $self->{_used} += 1;
}

#############################################################################
# Verify data written to VDO after resize.
##
sub _verifyData {
  my ($self, $device, $dataset) = assertNumArgs(3, @_);
  $log->info("Verifying $dataset->{tag}");
  $dataset->{slice}->verify();
}

#############################################################################
# Verify the single block that was written to VDO.
##
sub _verifyOneBlock {
  my ($self, $device, $dataset) = assertNumArgs(3, @_);
  $log->info("Verifying single block in $dataset->{tag}");
  $dataset->{singleBlockSlice}->verify();
}

#############################################################################
# Increase the physical size by one $RESIZE_INCREMENT.
##
sub _growPhysicalSpace {
  my ($self, $device) = assertNumArgs(2, @_);
  $log->info("Growing physical space");
  $self->{_vdoSize} += $RESIZE_INCREMENT;
  $device->growPhysical($self->{_vdoSize});
}

#############################################################################
# Perform clean shutdown and restart of the VDO.
##
sub _cleanShutdown {
  my ($self, $device) = assertNumArgs(2, @_);
  $log->info("Performing clean shutdown and restart VDO");
  $device->stop();
  $device->start();
}

#############################################################################
# Perform an emergency shutdown to trigger a rebuild.
##
sub _dirtyShutdown {
  my ($self, $device) = assertNumArgs(2, @_);
  $log->info("Simulating a crash");
  $self->emergencyRebootMachineForDevice($device);
  $device->waitUntilRecoveryComplete();
}

#############################################################################
# Test a VDO's physical growth by going through multiple scenarios,
# filling and growing, interlaced with clean shutdowns and rebuilds.
##
sub testGrowPhysicalTest02 {
  my ($self) = assertNumArgs(1, @_);

  my $device        = $self->getDevice();
  my $machine       = $device->getMachine();
  $self->{_vdoSize} = $self->{physicalSize};

  my $datasets = $self->_initializeDatasets($DATA_SET_COUNT);
  $log->info("Zeroing to ensure the block map is allocated");
  $device->ddWrite(
                   if    => "/dev/zero",
                   count => $self->{logicalSize} / $self->{blockSize},
                   bs    => $self->{blockSize},
                  );

  $log->info("Scenario 1: fill, resize, verify, write, clean restart");
  $self->_fillPhysicalSpace($device, $datasets->{0});
  $self->_growPhysicalSpace($device);
  $self->_verifyData($device, $datasets->{0});
  $self->_writeOneBlock($device, $datasets->{0});
  $self->_cleanShutdown($device);
  $self->_verifyOneBlock($device, $datasets->{0});

  $log->info("Scenario 2: fill, resize, verify, write, rebuild");
  $self->_fillPhysicalSpace($device, $datasets->{1});
  $self->_growPhysicalSpace($device);
  $self->_verifyData($device, $datasets->{1});
  $self->_writeOneBlock($device, $datasets->{1});
  $self->_verifyOneBlock($device, $datasets->{1});
  $self->_dirtyShutdown($device);
  $self->_verifyOneBlock($device, $datasets->{1});

  $log->info("Scenario 3: fill, resize, clean restart, verify, write");
  $self->_fillPhysicalSpace($device, $datasets->{2});
  $self->_growPhysicalSpace($device);
  $self->_cleanShutdown($device);
  $self->_verifyData($device, $datasets->{2});
  $self->_writeOneBlock($device, $datasets->{2});
  $self->_verifyOneBlock($device, $datasets->{2});

  $log->info("Scenario 4: fill, resize, rebuild, verify, write");
  $self->_fillPhysicalSpace($device, $datasets->{3});
  $self->_growPhysicalSpace($device);
  $self->_dirtyShutdown($device);
  $self->_verifyData($device, $datasets->{3});
  $self->_writeOneBlock($device, $datasets->{3});
  $self->_verifyOneBlock($device, $datasets->{3});

  $log->info("Scenario 5: fill, resize, verify, fill, verify");
  $self->_fillPhysicalSpace($device, $datasets->{4});
  $self->_growPhysicalSpace($device);
  $self->_verifyData($device, $datasets->{4});
  $self->_fillPhysicalSpace($device, $datasets->{5});

  $machine->dropCaches();
  $log->info("Verify all data written.");
  for (my $set = 0; $set < $DATA_SET_COUNT; $set++) {
    $self->_verifyData($device, $datasets->{$set});
  }
  $self->_verifyOneBlock($device, $datasets->{0});
  $self->_verifyOneBlock($device, $datasets->{1});
  $self->_verifyOneBlock($device, $datasets->{2});
  $self->_verifyOneBlock($device, $datasets->{3});
}

1;
