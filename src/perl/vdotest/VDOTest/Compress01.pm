##
# VDO test using compressed data
#
# $Id$
##
package VDOTest::Compress01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNear assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks to write
     blockCount        => 5000,
     # @ple Use a VDO device
     deviceType        => "lvmvdo",
     # @ple Enable compression on the VDO device
     enableCompression => 1,
     # @ple VDO slab bit count
     slabBits          => $SLAB_BITS_SMALL,
    );
##

########################################################################
# Basic VDO testing with compressible data.
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  my $stats = $device->getVDOStats();
  assertEqualNumeric(0, $stats->{"data blocks used"},
                     "Starting data blocks used should be zero");
  assertEqualNumeric(0, $stats->{"dedupe advice valid"},
                     "Starting dedupe advice valid should be zero");
  assertEqualNumeric(0, $stats->{"dedupe advice stale"},
                     "Starting dedupe advice stale should be zero");

  # Write some blocks, read them back and verify the data have not changed.
  my $dataset1 = $self->createSlice(blockCount => $self->{blockCount},
                                    blockSize  => $self->{blockSize},
                                   );
  # Use data compressible almost 4:1 (to pack 3:1 with overhead)
  $dataset1->write(tag => "d1", compress => .74, direct => 1);
  $dataset1->verify();

  # At 3:1 compression, blocks used should be approximately 1/3 of the total.
  # getVDOStats() issues a flush first, ensuring the statistics are correct.
  $stats = $device->getVDOStats();
  my $blocksUsed = $stats->{"data blocks used"};
  assertNear($self->{blockCount} / 3, $blocksUsed, 1,
             "Number of data blocks that should be compressed are");
  assertEqualNumeric(0, $stats->{"dedupe advice valid"},
                     "Dedupe advice valid should be zero");
  assertEqualNumeric(0, $stats->{"dedupe advice stale"},
                     "Dedupe advice stale should be zero");

  # Write the blocks again, expecting complete deduplication.
  my $dataset2 = $self->createSlice(blockCount => $self->{blockCount},
                                    blockSize  => $self->{blockSize},
                                    offset     => $self->{blockCount},
                                   );
  $dataset2->write(tag => "d1", compress => .74, direct => 1);

  $stats = $device->getVDOStats();
  $stats->logStats($device->getDevicePath());
  assertEqualNumeric($blocksUsed, $stats->{"data blocks used"},
                     "Data blocks used should not change");
  assertEqualNumeric($self->{blockCount}, $stats->{"dedupe advice valid"},
                     "Dedupe advice valid should be $self->{blockCount}");
  assertEqualNumeric(0, $stats->{"dedupe advice stale"},
                     "Dedupe advice stale should be zero");

  # Trim all the data and verify that the compressed blocks are reclaimed.
  $dataset1->trim();
  $dataset2->trim();

  $stats = $device->getVDOStats();
  assertEqualNumeric(0, $stats->{"data blocks used"},
                     "Data blocks used should be $self->{blockCount}");
}

1;
