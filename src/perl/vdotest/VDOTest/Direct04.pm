##
# Test deduping of data that are in flight.
#
# This test wants to put multiple writes in flight that are writing the
# same data.  The basic idea of the test is to write block X onto address A
# and at the same time write block X onto address B.
#
# $Id$
##
package VDOTest::Direct04;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants;
use POSIX qw(ceil);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks to write
     blockCount => 1000,
     # @ple Use a VDO device
     deviceType => "lvmvdo",
     # @ple VDO slab bit count
     slabBits   => $SLAB_BITS_SMALL,
    );
##

#############################################################################
# Write data that consist of alternating copies of two 4K blocks.
##
sub testSameBlocks {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  # Generate file of alternating blocks
  my $blockCount = 2;
  my $pathData = $self->generateDataFile($blockCount * $self->{blockSize});
  while ($blockCount < $self->{blockCount}) {
    $machine->dd(
                 if    => $pathData,
                 of    => $pathData,
                 count => $blockCount,
                 bs    => $self->{blockSize},
                 seek  => $blockCount,
                );
    $blockCount *= 2;
  }

  # Write the file of alternating blocks
  $device->ddWrite(
                   if    => $pathData,
                   count => $blockCount,
                   bs    => $self->{blockSize},
                   conv  => "fdatasync",
                  );
  $machine->dropCaches();

  # Read it back and verify that the data have not changed
  my $pathTemp = "${pathData}-temp";
  $device->ddRead(
                  of    => $pathTemp,
                  count => $blockCount,
                  bs    => $self->{blockSize},
                 );
  $machine->runSystemCmd("cmp $pathData $pathTemp");

  my $stats = $device->getVDOStats();
  my $expectedBlocksUsed = 2 * ceil(($blockCount / 2) / 254);
  assertEqualNumeric($expectedBlocksUsed, $stats->{"data blocks used"},
                     "Data blocks used should be $expectedBlocksUsed");
  assertEqualNumeric(0, $stats->{"dedupe advice stale"},
                     "Dedupe advice stale should be zero");
  assertEqualNumeric(0, $stats->{"dedupe advice timeouts"},
                     "Dedupe advice timeouts should be zero");
  assertEqualNumeric($blockCount,
                     $stats->{"bios in write"},
                     "Inbound writes should be block count only.");
  assertEqualNumeric($expectedBlocksUsed, $stats->{"bios out write"},
                     "Outbound writes should be unique blocks.");
}

1;
