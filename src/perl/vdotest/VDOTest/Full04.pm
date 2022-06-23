##
# Test VDO behavior when VDO runs out of physical space, using page cache I/O.
#
# We allocate 4 non-overlapping slices (or partitions) of the VDO device,
# where the entire device has only enough physical storage to accomodate
# one of the slices.  In parallel we write data to fill each slice.  This
# will fail silently, because the I/O error occurs when the page cache is
# flushed.  We then read and verify the data that were written.
#
# Because we can make two two-way choices, we run this test 4 ways:
# - We run both with and without dedupe.
# - We run both with and without compression.
#
# $Id$
##
package VDOTest::Full04;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest::FullBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# Test with no dedupe, no compression
##
sub testVanilla {
  my ($self) = assertNumArgs(1, @_);
  $self->_runOutOfSpace(0, 0);
}

########################################################################
# Test with 50% dedupe, no compression
##
sub testDedupe {
  my ($self) = assertNumArgs(1, @_);
  $self->_runOutOfSpace(0.5, 0);
}

########################################################################
# Test with no dedupe, 60% compression
##
sub testCompress {
  my ($self) = assertNumArgs(1, @_);
  $self->_runOutOfSpace(0, 0.6);
}

########################################################################
# Test with 33% dedupe, 60% compression
##
sub testCompressAndDedupe {
  my ($self) = assertNumArgs(1, @_);
  $self->_runOutOfSpace(0.334, 0.6);
}

########################################################################
##
sub _runOutOfSpace {
  my ($self, $dedupeFraction, $compressFraction) = assertNumArgs(3, @_);
  my $device = $self->getDevice();
  my $SLICE_COUNT = 4;

  my $stats = $device->getVDOStats();
  my $blockCount = int($self->getUsableDataBlocks($stats) / 1000) * 1000;
  $log->info("Block count: $blockCount");
  $self->{_blockCount} = $blockCount;
  $self->{_blockSize}  = $stats->{"block size"};

  # Create the slices, and write data to each slice
  my (@slices, @tasks);
  for my $number (1 .. $SLICE_COUNT) {
    my $offset = ($number - 1) * $self->{_blockCount};
    my $slice = $self->createSlice(
                                   blockCount => $self->{_blockCount},
                                   blockSize  => $self->{_blockSize},
                                   offset     => $offset,
                                  );
    push(@slices, $slice);
    my %args = (
                compress => $compressFraction,
                dedupe   => $dedupeFraction,
                tag      => "data$number",
               );
    push(@tasks,
         Permabit::VDOTask::SliceOperation->new($slice, "write", %args));
  }
  $self->getAsyncTasks()->addTasks(@tasks);
  map { $_->start() } @tasks;
  map { $_->result() } @tasks;

  # Sync the data.
  $self->syncDeviceIgnoringErrors();

  # Force the data out of the page cache.  There seems to be a sticky write
  # error condition.  If we try to verify the data now, then we will get EIO
  # errors when we do the read.  So we must take extreme measures to eliminate
  # any traces of the past lingering in the page cache.
  $device->restart();

  # Verify each section.
  $self->verifySlices(\@slices);
}

1;
