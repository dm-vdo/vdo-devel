##
# Test system behavior when VDO runs out of physical space.
#
# The major points to this test is that we can fill the VDO device, and
# subsequent writes will produce a predictable error.
#
# $Id$
##
package VDOTest::Full01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);

use base qw(VDOTest::FullBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
##
sub testNoSpace {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  my $stats = $device->getVDOStats();
  my $blockCount = $self->getUsableDataBlocks($stats);
  $log->info("Physical Blocks: $stats->{'physical blocks'}");
  $log->info("Overhead Blocks: $stats->{'overhead blocks used'}");
  $log->info("Data Blocks:     $stats->{'data blocks used'}");
  $log->info("Logical Blocks:  $stats->{'logical blocks'}");
  $log->info("Block Size:      $stats->{'block size'}");
  $log->info("Usable Blocks:   $blockCount");

  # Fill the device using direct writes
  my $dataSlice = $self->createSlice(blockCount => $blockCount,
                                     blockSize  => $stats->{"block size"},);
  $dataSlice->writeENOSPC(
                          tag    => "data",
                          direct => 1,
                         );

  $stats = $device->getVDOStats();
  $blockCount = $self->getUsableDataBlocks($stats);
  $log->info("Overhead Blocks: $stats->{'overhead blocks used'}");
  $log->info("Data Blocks:     $stats->{'data blocks used'}");
  $log->info("Usable Blocks:   $blockCount");
  assertEqualNumeric($stats->getFreeBlocks(), 0, "Device is full");

  my %expectStats
    = (
       "physical blocks"      => $stats->{"physical blocks"},
       "overhead blocks used" => $stats->{"overhead blocks used"},
       "data blocks used"     => $blockCount,
       "logical blocks"       => $stats->{"logical blocks"},
       "logical blocks used"  => $blockCount,
      );

  # Try to write 1 more block using a direct write.  This will fail because
  # we are out of space.
  my $oneSlice = $self->createSlice(blockCount => 1,
                                    blockSize  => $stats->{"block size"},
                                    offset     => $blockCount,);
  $oneSlice->writeENOSPC(direct => 1, tag => "direct1");
  $self->assertVDOStats(\%expectStats);

  # Read and verify the device.  Only the first slice really got written.
  $dataSlice->verify();
}

1;
