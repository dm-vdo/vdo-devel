##
# Test overwriting of data with identical data.
#
# This test wants to write a set of data blocks once, and then write them
# a second time to the same addresses.  The basic idea of the test is to
# write block X onto address A, and then write block X again onto address
# A.
#
# $Id$
##
package VDOTest::Direct02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants;

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
# Overwrite blocks with identical data
##
sub testTwice {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $slice = $self->createSlice(blockCount => $self->{blockCount});

  $log->info("First write and verify");
  $slice->write(tag    => "Direct2",
                direct => 1,);
  $slice->verify();

  my $stats = $device->getVDOStats();
  assertEqualNumeric($self->{blockCount}, $stats->{"data blocks used"},
                     "Data blocks used should be block count");
  assertEqualNumeric(0, $stats->{"dedupe advice valid"},
                     "Dedupe advice valid should be zero");
  assertEqualNumeric(0, $stats->{"dedupe advice stale"},
                     "Dedupe advice stale should be zero");
  assertEqualNumeric(0, $stats->{"dedupe advice timeouts"},
                     "Dedupe advice timeouts should be zero");
  assertEqualNumeric($self->{blockCount}, $stats->{"bios in write"},
                     "Inbound writes should be block count");
  assertEqualNumeric($self->{blockCount}, $stats->{"bios out write"},
                     "Outbound writes should be block count");

  # Overwrites work a little differently in Lithium, in that there are no
  # overwrites. We always write to a new unreferenced block first, which means
  # the original block may gets its reference count set to 0 and then get
  # reused by a future write. This test needs to change based on this new
  # setup.
  $log->info("Second write and verify");
  $slice->write(tag    => "Direct2",
                direct => 1,);
  $slice->verify();

  $stats = $device->getVDOStats();
  assertEqualNumeric($self->{blockCount}, $stats->{"data blocks used"},
                     "Data blocks used should equal the block count");
  assertEqualNumeric($self->{blockCount}, $stats->{"dedupe advice valid"},
                     "Dedupe advice valid should equal the block count");
  assertEqualNumeric(0, $stats->{"dedupe advice stale"},
                     "Dedupe advice stale should be zero");
  assertEqualNumeric(0, $stats->{"dedupe advice timeouts"},
                     "Dedupe advice timeouts should be zero");
  assertEqualNumeric(2 * $self->{blockCount}, $stats->{"bios in write"},
                     "Inbound writes should equal twice the block count");
  assertEqualNumeric($self->{blockCount}, $stats->{"bios out write"},
                     "Outbound writes should be block count");
}

1;
