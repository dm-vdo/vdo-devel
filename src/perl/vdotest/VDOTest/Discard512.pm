##
# Test discarding blocks when using logical 512 byte blocks.
#
# $Id$
##
package VDOTest::Discard512;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants qw($SECTOR_SIZE $SECTORS_PER_BLOCK);
use Permabit::Utils qw(makeFullPath);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDO device
     deviceType         => "lvmvdo",
     # @ple Emulate a 512 byte block device
     emulate512Enabled  => 1,
     # @ple Enable compression
     enableCompression  => 1,
    );
##

#############################################################################
# Assert that the number of blocks used has the expected value.
##
sub assertLogicalBlocksUsed {
  my ($self, $expected) = assertNumArgs(2, @_);
  my $blocksUsed = $self->getDevice()->getVDOStats()->{"logical blocks used"};
  assertEqualNumeric($blocksUsed, $expected);
}

#############################################################################
# Discard some blocks on the VDO device.
##
sub discardSomeBlocks {
  my ($self) = assertNumArgs(1, @_);

  my $device = $self->getDevice();
  my $machine = $self->getDevice()->getMachine();
  foreach my $extent (@{$self->{discardExtents}}) {
    $machine->genDiscard(
                         of    => $device->getSymbolicPath(),
                         bs    => $SECTOR_SIZE,
                         seek  => $extent->{offset},
                         count => $extent->{length},
                        );
  }
}

#############################################################################
# Zero out some blocks on the reference copy of the data.
##
sub zeroSomeBlocks {
  my ($self, $file) = assertNumArgs(2, @_);

  my $machine = $self->getDevice()->getMachine();
  foreach my $extent (@{$self->{discardExtents}}) {
    $machine->dd(
                 if    => "/dev/zero",
                 of    => $file,
                 bs    => $SECTOR_SIZE,
                 seek  => $extent->{offset},
                 count => $extent->{length},
                 conv  => "notrunc",
                );
  }
}

#############################################################################
# Generate a list of extents which describe the regions to discard.
# Each extent contains an offset and a length. Also generate counts
# for the total number of blocks written, and the number of full
# blocks discarded.
##
sub constructDiscardList {
  my ($self) = assertNumArgs(1, @_);
  my @extents = ();
  # The sum of the lengths in this list is an exact multiple of
  # $SECTORS_PER_BLOCK, plus one extra block. This ensures that the
  # offset shifting below works correctly.
  #
  # The list includes short extents, plus extents a few blocks long to
  # test the handling of fully discarded blocks in the middle of
  # extents (between possible partially-discarded leading and trailing
  # sections).
  my @extentLengths
    = (
       1, 2, $SECTORS_PER_BLOCK - 2,
       map {
         my $numSectors = $SECTORS_PER_BLOCK * $_;
         ($numSectors - 1, $numSectors, $numSectors + 1);
       } (1..4),
      );

  # The inner loop adds a number of full blocks, plus a single sector,
  # which causes the outer loop to repeat the pattern at each possible
  # offset from the 4K block boundary.
  my $currentOffset = 0;
  my $discardedBlocks = 0;
  foreach my $i (1 .. $SECTORS_PER_BLOCK) {
    foreach my $length (@extentLengths) {
      # Add some non-discarded space before each extent.
      $currentOffset += 2 * $SECTORS_PER_BLOCK;
      push(@extents, { offset => $currentOffset, length => $length });
      my $db = 0;
      # Determine how many full blocks will be discarded.
      if ($length >= $SECTORS_PER_BLOCK) {
        my $lastWrittenOffset = ($currentOffset - 1) % $SECTORS_PER_BLOCK;
        my $firstBlockSectors = $SECTORS_PER_BLOCK - $lastWrittenOffset - 1;
        $db = int(($length - $firstBlockSectors) / $SECTORS_PER_BLOCK);
        $discardedBlocks
          += int(($length - $firstBlockSectors) / $SECTORS_PER_BLOCK);
      }
      $currentOffset += $length;
    }
  }

  $self->{blockCount}      = int(($currentOffset / $SECTORS_PER_BLOCK) + 3);
  $self->{discardedBlocks} = $discardedBlocks;
  $self->{discardExtents}  = \@extents;
}

#############################################################################
##
sub testDiscard {
  my ($self) = assertNumArgs(1, @_);

  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  $self->constructDiscardList();

  # Write some arbitrary data to VDO so there is something to discard.
  # Use oflag=direct so that the logical blocks used assertion will work
  # when the PAGESIZE is not 4K.
  my $fileSize    = $self->{blockCount} * $self->{blockSize};
  my $sectorCount = $self->{blockCount} * $SECTORS_PER_BLOCK;
  my $source      = $self->generateDataFile($fileSize, "testdata");
  $device->ddWrite(
                   oflag => "direct",
                   bs    => $SECTOR_SIZE,
                   count => $sectorCount,
                   if    => $source,
                  );
  $self->assertLogicalBlocksUsed($self->{blockCount});

  # Discard the specified extents and check that nothing else disappeared.
  $self->discardSomeBlocks();
  $self->zeroSomeBlocks($source);
  $log->debug("Used $self->{blockCount} blocks; "
              . "discarded $self->{discardedBlocks} blocks.");
  my $expectedLogicalBlocks = $self->{blockCount} - $self->{discardedBlocks};
  $self->assertLogicalBlocksUsed($expectedLogicalBlocks);

  # Verify that the discards were done correctly.
  # Use iflag=direct for consistency with the writing of the device.
  my $verifyPath = makeFullPath($machine->getScratchDir(), "deviceCopy");
  $device->ddRead(
                  iflag => "direct",
                  bs    => $SECTOR_SIZE,
                  count => $sectorCount,
                  of    => $verifyPath,
                 );
  $machine->runSystemCmd("cmp $source $verifyPath");
}
