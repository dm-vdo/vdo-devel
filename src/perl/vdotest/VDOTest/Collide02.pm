##
# Write N blocks of data where there is no dedupe but every block has the
# same Murmur3 hash signature.
#
# $Id$
##
package VDOTest::Collide02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks in a dataset
     blockCount => 1000000,
     # @ple Use a VDO device
     deviceType => "lvmvdo",
    );
##

########################################################################
##
sub testManyCollisions {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $dedupeAdvicePredictable = $self->canPredictBlockUsage();

  my $stats = $device->getVDOStats();
  assertEqualNumeric(0, $stats->{"data blocks used"});
  assertEqualNumeric(0, $stats->{"dedupe advice valid"});
  assertEqualNumeric(0, $stats->{"dedupe advice stale"});
  assertEqualNumeric(0, $stats->{"entries indexed"});

  # Write a single data block.  Then write N-1 blocks that have the same hash
  # signature as the first block.
  my $slice = $self->createSlice(blockCount => 1);
  $slice->write(
                tag   => "Single",
                fsync => 1,
               );
  $machine->murmur3collide(
                           if    => $device->getSymbolicPath(),
                           count => $self->{blockCount} - 1,
                           bs    => $slice->{blockSize},
                           skip  => $slice->{offset},
                           of    => $device->getSymbolicPath(),
                           seek  => $slice->{offset} + 1,
                           fsync => 1,
                          );
  $stats = $device->getVDOStats();
  # Check for dedupe timeouts first.  If there are any, the other expected
  # values will also be wrong.
  assertEqualNumeric(0, $stats->{"dedupe advice timeouts"});
  # Check for values that are consistent with no dedupe and 100% hash
  # collisions with the first block
  assertEqualNumeric(1, $stats->{"entries indexed"});
  assertEqualNumeric($self->{blockCount}, $stats->{"data blocks used"});
  if ($dedupeAdvicePredictable) {
    assertEqualNumeric(0, $stats->{"dedupe advice valid"});
    # Collisions appear to be stale advice except when they're detected in
    # concurrent writes. The first write is neither.
    assertEqualNumeric($self->{blockCount} - 1,
                       $stats->{"dedupe advice stale"}
                       + $stats->{"concurrent hash collisions"});
  }

  # Verify the data can be read back
  $machine->dropCaches();
  $slice->verify();
  $machine->murmur3collide(
                           if     => $device->getSymbolicPath(),
                           count  => $self->{blockCount} - 1,
                           bs     => $slice->{blockSize},
                           skip   => $slice->{offset},
                           of     => $device->getSymbolicPath(),
                           seek   => $slice->{offset} + 1,
                           verify => 1,
                          );
}

1;
