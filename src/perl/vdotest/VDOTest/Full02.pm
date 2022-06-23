##
# Test VDO behavior when VDO runs out of physical space, using direct I/O.
#
# We allocate 4 non-overlapping slices (or partitions) of the VDO device,
# where the entire device has only enough physical storage to accomodate
# one of the slices.  In parallel we write data to each slice until we get
# an error (because VDO is full).  We then read and verify the data that
# were written.
#
# Because we can make two two-way choices, we run this test 4 ways:
# - We run both with and without dedupe.
# - We run both with and without compression.
#
# $Id$
##
package VDOTest::Full02;

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
  my $SLICE_COUNT = 4;

  my $stats = $self->getDevice()->getVDOStats();
  my $blockCount = int($self->getUsableDataBlocks($stats) / 1000) * 1000;
  $log->info("Block count: $blockCount");
  $self->{_blockCount}   = $blockCount;
  $self->{_blockSize}    = $stats->{"block size"};

  # Create the slices, and tasks to write data to each slice
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
                direct   => 1,
                tag      => "data$number",
               );
    push(@tasks,
         Permabit::VDOTask::SliceOperation->new($slice, "writeENOSPC", %args));
  }
  $self->getAsyncTasks()->addTasks(@tasks);
  map { $_->start() } @tasks;
  map { $_->result() } @tasks;

  # Verify each section.
  $self->verifySlices(\@slices);
}

1;
