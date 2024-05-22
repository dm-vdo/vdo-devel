##
# Class for testing VDO rebuild time on a large empty logical
# address space.
#
# $Id$
##
package VDOTest::LargeRebuild;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest::RebuildBase);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple how many blocks to write at once
     blockCount        => 256 * 1024,
     # @ple how large the block map cache is
     blockMapCacheSize => 5 * $GB,
     # @ple class of machine, requires large volume
     clientClass       => "VDO-PMI",
     # @ple device stack must be lvmvdo-linear at present; due to
     #      physical size, must have a linear just under VDO, and
     #      RebuildBase is not yet smart enough to start multiple
     #      devices atop Dory.
     deviceType        => "lvmvdo-linear",
     # @ple volume size
     logicalSize       => 256 * $TB,
     # @ple physical size
     physicalSize      => 768 * $GB,
     # @ple Use the largest possible slabs for performance tests
     slabBits          => $SLAB_BITS_LARGE,
     # @ple Whether to use a file system
     useFilesystem     => 0,
    );
##

#############################################################################
# ESC-531:
# This test case creates an empty but large logical address space vdo,
# writes a little bit to it sequentially, crashes, then rebuilds the vdo.
# The metric is how long the rebuild takes. Prior to change 101194, rebuilding
# uninitialized block map pages could take a long time since each page
# immediately became dirty and had to be re-written.
#
# Since the change, uninitialized pages are not marked dirty unless they are
# written to, so that simple scanning by the recycler or the rebuild logic
# does not cause the entire block map to be rewritten.
##
sub testRebuildEmpty {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  $self->createSlice(
                     blockCount => $self->{blockCount},
                     blockSize  => $self->{blockSize},
                    )->write(tag => "write", direct => 1);
  $self->recoverAndRestartVDO();
}

#############################################################################
# Write chunks of null data in various locations, then crash. This maximizes
# number of journal entries, which is one worst possible case for rebuild after
# change 103987.
# To get maximal journal entries, we want the block map cache to be large,
# and then we want to get as many writes in before the block map cache
# flushes any pages to disk or starts reaping journal entries.
##
sub testRebuildRandomWrites {
  my ($self) = assertNumArgs(1, @_);
  my $device       = $self->getDevice();
  my $dataSize     = $self->{blockCount} * $self->{blockSize};
  # How far to jump between writes.
  my $strideLength = 10;
  my $iterations   = 14;
  # We read the entire set of pages we're going to write, to pre-fill the
  # block map cache.
  for (my $i = 0; $i < $iterations; $i++) {
    $device->ddRead(of    => "/dev/null",
                    count => 1,
                    bs    => $dataSize,
                    skip  => $i * $strideLength);
  }
  # Zero writes are maximally fast, and we wish to perform as many writes as
  # feasible before the journal starts being reaped.
  for (my $i = 0; $i < $iterations; $i++) {
    $device->ddWrite(if    => "/dev/zero",
                     count => 1,
                     bs    => $dataSize,
                     seek  => $i * $strideLength,
                     oflag => "direct");
  }
  $self->recoverAndRestartVDO();
}

1;
