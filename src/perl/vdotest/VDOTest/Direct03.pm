##
# Basic VDO test using block discard
#
# We test that VDO is counting the correct number of discard bios.  We test
# that a block that is discarded reads as a zero block.  We test that
# discarding a block removes it from the "data blocks used" count.
#
# $Id$
##
package VDOTest::Direct03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants;
use POSIX qw(ceil);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of data blocks to use
     blockCount  => 1024,
     # @ple Use a VDO device
     deviceType  => "lvmvdo",
     # @ple logical (provisioned) size of the exported VDO device
     logicalSize => 512 * $MB,
    );
##

########################################################################
# Test trimming data.
##
sub testDiscard {
  my ($self) = assertNumArgs(1, @_);
  $self->_discardBlocks();
  $self->_discardDuplicatedBlocks();
  $self->_discardWithHoles();
}

########################################################################
# Write 3*N blocks of data, then trim away the middle N blocks.
##
sub _discardWithHoles {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  my $beforeStats = $device->getVDOStats();
  assertEqualNumeric(0, $beforeStats->{"data blocks used"},
                     "Starting data blocks used should be zero");

  # Write three datasets of random data.
  my $datasets = [
                  $self->createSlice(
                                     blockCount => $self->{blockCount},
                                     blockSize  => $self->{blockSize},
                                    ),
                  $self->createSlice(
                                     blockCount => $self->{blockCount},
                                     blockSize  => $self->{blockSize},
                                     offset     => $self->{blockCount},
                                    ),
                  $self->createSlice(
                                     blockCount => $self->{blockCount},
                                     blockSize  => $self->{blockSize},
                                     offset     => 2 * $self->{blockCount},
                                    ),
                 ];
  $datasets->[0]->write(tag => "notrim1", fsync => 1);
  $datasets->[1]->write(tag => "trim", fsync => 1);
  $datasets->[2]->write(tag => "notrim2", fsync => 1);
  $machine->dropCaches();

  my $stats = $device->getVDOStats();
  assertEqualNumeric(3 * $self->{blockCount}, $stats->{"data blocks used"},
                     "Data blocks used should be thrice $self->{blockCount}");

  # Trim the second dataset
  $datasets->[1]->trim();
  $stats = $device->getVDOStats();
  assertEqualNumeric(2 * $self->{blockCount}, $stats->{"data blocks used"},
                     "Data blocks used should be twice $self->{blockCount}");
  $stats = $stats - $beforeStats;

  # VDO-1922: We've changed the way we handle discards. We're allowing up to
  # MAX_DISCARD_BLOCKS worth of blocks per discard bio.
  my $discards = ceil($self->{blockCount} / $self->getMaxDiscardBlocks());
  assertEqualNumeric($discards, $stats->{"bios in discard"},
                     "Should have processed $discards discard bios");

  # Verify the trimmed and untrimmed data.
  $datasets->[0]->verify();
  $datasets->[1]->verify();
  $datasets->[2]->verify();
}

########################################################################
# Write blocks of data, then trim it away.
##
sub _discardBlocks {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  my $beforeStats = $device->getVDOStats();
  assertEqualNumeric(0, $beforeStats->{"data blocks used"},
                     "Starting data blocks used should be zero");

  my $dataset = $self->createSlice(
                                   blockCount => $self->{blockCount},
                                   blockSize  => $self->{blockSize},
                                  );
  # Write the data, read it back and verify the data have not changed.
  $dataset->write(tag => "discard");
  $machine->dropCaches();
  $dataset->verify();
  my $stats = $device->getVDOStats();
  assertEqualNumeric($self->{blockCount}, $stats->{"data blocks used"},
                     "Data blocks used should be $self->{blockCount}");

  # Trim the data, read it back and verify the data are now zero
  my $devPath = $device->getSymbolicPath();
  $dataset->trim();
  $dataset->verify();
  $stats = $device->getVDOStats();
  assertEqualNumeric(0, $stats->{"data blocks used"},
                     "Starting data blocks used should be zero again");
  my $duringStats = $stats - $beforeStats;

  # VDO-1922: We've changed the way we handle discards. We're allowing up to
  # MAX_DISCARD_BLOCKS worth of blocks per discard bio.
  my $discards = ceil($self->{blockCount} / $self->getMaxDiscardBlocks());
  assertEqualNumeric($discards, $duringStats->{"bios in discard"},
                     "Should have processed $discards discard bios");
}

########################################################################
# Write blocks of duplicated data, then trim it away.
##
sub _discardDuplicatedBlocks {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  my $beforeStats = $device->getVDOStats();
  assertEqualNumeric(0, $beforeStats->{"data blocks used"},
                     "Starting data blocks used should be zero");

  my $datasets = [
                  $self->createSlice(
                                     blockCount => $self->{blockCount},
                                     blockSize  => $self->{blockSize},
                                    ),
                  $self->createSlice(
                                     blockCount => $self->{blockCount},
                                     blockSize  => $self->{blockSize},
                                     offset     => $self->{blockCount},
                                    ),
                 ];
  # Write the data once, then write the same data again.
  $datasets->[0]->write(tag => "dupe");
  $datasets->[1]->write(tag => "dupe");
  # Verify the data has/have not changed
  $machine->dropCaches();
  $datasets->[0]->verify();
  $datasets->[1]->verify();
  my $stats = $device->getVDOStats();
  assertEqualNumeric($self->{blockCount}, $stats->{"data blocks used"},
                     "Data blocks used should be $self->{blockCount}");

  # Trim one copy, read it back and verify the data are now zero
  $datasets->[0]->trim();
  $machine->dropCaches();
  $datasets->[0]->verify();
  $stats = $device->getVDOStats();
  assertEqualNumeric($self->{blockCount}, $stats->{"data blocks used"},
                     "Data blocks used should be $self->{blockCount}");
  $stats = $stats - $beforeStats;

  # VDO-1922: We've changed the way we handle discards. We're allowing up to
  # MAX_DISCARD_BLOCKS worth of blocks per discard bio.
  my $discards = ceil($self->{blockCount} / $self->getMaxDiscardBlocks());
  assertEqualNumeric($discards, $stats->{"bios in discard"},
                     "Should have processed $discards discard bios");

  # Trim the second copy, read it back and verify the data is/are now zero
  $datasets->[1]->trim();
  $machine->dropCaches();
  $datasets->[1]->verify();
  $stats = $device->getVDOStats();
  assertEqualNumeric(0, $stats->{"data blocks used"},
                     "Starting data blocks used should be zero again");
  $stats = $stats - $beforeStats;

  # VDO-1922: We've changed the way we handle discards. We're allowing up to
  # MAX_DISCARD_BLOCKS worth of blocks per discard bio.
  $discards = 2 * ceil($self->{blockCount} / $self->getMaxDiscardBlocks());
  assertEqualNumeric($discards, $stats->{"bios in discard"},
                     "Should have processed $discards discard bios");
}

########################################################################
# Get the maximum discard blocks
##
sub getMaxDiscardBlocks {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  return $device->getMaxDiscardSectors() * $SECTOR_SIZE / $self->{blockSize};
}

1;
