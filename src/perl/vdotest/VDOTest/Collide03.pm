##
# Write N blocks of data where there is no dedupe but every block has the
# same Murmur3 hash signature.  Use blocks that compress really well.
#
# $Id$
##
package VDOTest::Collide03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertLENumeric assertNumArgs);
use Time::HiRes qw(sleep);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks in a dataset
     blockCount        => 1000000,
     # @ple Use a VDO device
     deviceType        => "lvmvdo",
     # @ple Enable compression on the VDO device
     enableCompression => 1,
    );
##

########################################################################
##
sub testCompressingCollisions {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $dedupeAdvicePredictable = $self->canPredictBlockUsage();

  my $stats = $device->getVDOStats();
  assertEqualNumeric(0, $stats->{"data blocks used"});
  assertEqualNumeric(0, $stats->{"dedupe advice valid"});
  assertEqualNumeric(0, $stats->{"dedupe advice stale"});
  assertEqualNumeric(0, $stats->{"entries indexed"});

  # Write a single data block that has incredible compressibility.  Then write
  # N-1 blocks that have the same hash signature and compressibility as the
  # first block.
  $device->ddWrite(
                   if    => "/dev/zero",
                   count => 1,
                   bs    => $self->{blockSize},
                   conv  => "fdatasync",
                  );
  $machine->murmur3collide(
                           if    => $device->getSymbolicPath(),
                           count => $self->{blockCount} - 1,
                           bs    => $self->{blockSize},
                           of    => $device->getSymbolicPath(),
                           seek  => 1,
                           fsync => 1,
                          );

  # Wait for VDO to get every incomplete block into the packer.
  for (;;) {
    $stats = $device->getCurrentVDOStats();
    my $workingBlocks = ($stats->{"current VDO IO requests in progress"}
                         - $stats->{"compressed fragments in packer"});
    if ($workingBlocks == 0) {
      last;
    }
    $log->info("$workingBlocks blocks have not gotten to the packer yet");
    sleep(0.1);
  }

  # Record how many blocks are still in the packer.  If there is only one
  # such block, it will fail to be written as a compressed fragment when we
  # flush the packer.  If there is more than one such block, it is possible
  # that work in progress will lower the count to one.  There is a race
  # here, but rather than a short sleep and a prayer, we will just assume
  # that there is a possibiity of a single leftover fragment whenever there
  # are any blocks left in the packer.
  my $packerCount = $stats->{"compressed fragments in packer"};
  # This call to get the VDOStats will have the side effect of flushing the
  # packer.
  $stats = $device->getVDOStats();
  # Check for dedupe timeouts first.  If there are any, the other expected
  # values will also be wrong.
  assertEqualNumeric(0, $stats->{"dedupe advice timeouts"});
  # Check for values that are consistent with no dedupe.
  if ($dedupeAdvicePredictable) {
    assertEqualNumeric(0, $stats->{"dedupe advice valid"});
  }
  # Check for index entries.  There has only been 1 hash value in all the data.
  assertEqualNumeric(1, $stats->{"entries indexed"});

  # Verify the data can be read back
  $machine->dropCaches();
  $machine->murmur3collide(
                           if     => $device->getSymbolicPath(),
                           count  => $self->{blockCount} - 1,
                           bs     => $self->{blockSize},
                           of     => $device->getSymbolicPath(),
                           seek   => 1,
                           verify => 1,
                          );
}

1;
