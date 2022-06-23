##
# Write N blocks of data, and write another N blocks of data that use
# differing data but the same Murmur3 hashes.
#
# $Id$
##
package VDOTest::Collide01;

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
     blockCount  => 1000000,
     # @ple Use a VDO device
     deviceType  => "lvmvdo",
     # @ple logical (provisioned) size of the exported VDO device
     logicalSize => "30G",
    );
##

########################################################################
##
sub testTwoSets {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $dedupeAdvicePredictable = $self->canPredictBlockUsage();

  # Verify initial statistics
  my $stats = $device->getVDOStats();
  assertEqualNumeric(0, $stats->{"data blocks used"});
  assertEqualNumeric(0, $stats->{"dedupe advice valid"});
  assertEqualNumeric(0, $stats->{"dedupe advice stale"});
  assertEqualNumeric(0, $stats->{"entries indexed"});

  # Write the set of data blocks.
  my $slice = $self->createSlice(blockCount => $self->{blockCount});
  $slice->write(
                tag   => "First",
                fsync => 1,
               );
  $stats = $device->getVDOStats();
  # Check for dedupe timeouts first.  If there are any, the other expected
  # values will also be wrong.
  assertEqualNumeric(0, $stats->{"dedupe advice timeouts"});
  # Check for values that are consistent with no dedupe and no hash collisions
  assertEqualNumeric($self->{blockCount}, $stats->{"data blocks used"});
  assertEqualNumeric(0,                   $stats->{"dedupe advice stale"});
  assertEqualNumeric($self->{blockCount}, $stats->{"entries indexed"});
  if ($dedupeAdvicePredictable) {
    assertEqualNumeric(0, $stats->{"dedupe advice valid"});
  }

  # Write the second set of blocks.  The two sets are completely disjoint, but
  # hash to identical hash names.
  $machine->murmur3collide(
                           if    => $device->getSymbolicPath(),
                           count => $self->{blockCount},
                           bs    => $slice->{blockSize},
                           skip  => $slice->{offset},
                           of    => $device->getSymbolicPath(),
                           seek  => $self->{blockCount},
                           fsync => 1,
                          );
  $stats = $device->getVDOStats();
  # Check for values that are consistent with no dedupe and 100% hash
  # collisions with the first dataset
  assertEqualNumeric(2 * $self->{blockCount}, $stats->{"data blocks used"});
  assertEqualNumeric($self->{blockCount},     $stats->{"entries indexed"});
  if ($dedupeAdvicePredictable) {
    assertEqualNumeric(0, $stats->{"dedupe advice valid"});
  }
  # Every block in the second set should either be stale advice or no advice
  # due to a timeout.
  assertEqualNumeric($self->{blockCount},
                     $stats->{"dedupe advice stale"}
                     + $stats->{"dedupe advice timeouts"});

  # Verify the data can be read back
  $machine->dropCaches();
  $slice->verify();
  $machine->murmur3collide(
                           if     => $device->getSymbolicPath(),
                           count  => $self->{blockCount},
                           bs     => $slice->{blockSize},
                           skip   => $slice->{offset},
                           of     => $device->getSymbolicPath(),
                           seek   => $self->{blockCount},
                           verify => 1,
                          );
}

1;
