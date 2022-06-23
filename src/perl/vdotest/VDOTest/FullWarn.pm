##
# Test system behavior when VDO nears running out of physical space.
#
# The major points to this test are:
#
# - As we fill the device, we will check dmsetup status and look at the
#   information to determine if we are getting close to full.
#
# - We will log warnings at 50%, 75%, 90% and 100%.
#
# $Id$
##
package VDOTest::FullWarn;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertLTNumeric assertNumArgs);
use Permabit::Utils qw(retryUntilTimeout);

use base qw(VDOTest::FullBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
##
sub testNoSpace {
  my ($self) = assertNumArgs(1, @_);

  my $device = $self->getDevice();
  my $stats = $device->getVDOStats();
  my $blockCount = $self->getUsableDataBlocks($stats);
  $log->info("Block Size:      $stats->{'block size'}");
  $log->info("Physical Blocks: $stats->{'physical blocks'}");
  $log->info("Overhead Blocks: $stats->{'overhead blocks used'}");
  $log->info("Data Blocks:     $stats->{'data blocks used'}");
  $log->info("Usable Blocks:   $blockCount");

  # Fill the device using direct writes
  my $slice = $self->createSlice(blockCount => $blockCount,
                                 blockSize  => $stats->{"block size"},);
  $slice->writeENOSPC(tag => "data", direct => 1);

  $stats = $device->getVDOStats();
  $blockCount = $self->getUsableDataBlocks($stats);
  $log->info("Overhead Blocks: $stats->{'overhead blocks used'}");
  $log->info("Data Blocks:     $stats->{'data blocks used'}");
  $log->info("Usable Blocks:   $blockCount");
  assertEqualNumeric($stats->getFreeBlocks(), 0, "Device is full");

  # Now make sure dmsetup status shows it is full too
  my $output = $device->getStatus();

  # <start> <length> <type> <dev> <mode> <recovery> <index>
  # <compress> <used> <total>
  my @fields = split(' ', $output);

  my $used  = $fields[8];
  my $total = $fields[9];
  my $freeBlocks = $total - $used;
  assertEqualNumeric($freeBlocks, 0, "dmsetup status says device is full");
}

1;
