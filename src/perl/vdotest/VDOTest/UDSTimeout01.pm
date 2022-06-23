##
# Write a data stream that will cause dedupe timeouts to occur.
#
# $Id$
##
package VDOTest::UDSTimeout01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertLTNumeric assertNumArgs);
use Permabit::Constants;
use Permabit::GenSlice;
use Permabit::Utils qw(parseBytes);
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDO device stacked upon a delay device
     deviceType         => "vdo-delay",
     # @ple Use a short deduplication timeout.  It is in milliseconds.
     vdoAlbireoTimeout  => 100,
    );
##

########################################################################
# Create a filesystem, generate data into it in parallel from
# different streams, and then verify it.
##
sub testTimeout {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  # Write ten datasets.  We want each dataset to be large enough to occupy more
  # than a single chapter of the index.  Reserve space for two slices to
  # contain each dataset.
  my $blockCount = 150000;
  foreach my $number (0 .. 9) {
    my $tag = "D$number";
    my $offset = 2 * $number * $blockCount;
    # Write one copy of the dataset.  We want to write them one at a time so
    # that they are not intermingled in the index.
    my $slice = Permabit::GenSlice->new(device     => $device,
                                        blockCount => $blockCount,
                                        offset     => $offset);
    $slice->write(tag => $tag, fsync => 1);
    # Set up the AsyncTask that will write the second copy of the dataset.
    $slice = Permabit::GenSlice->new(device     => $device,
                                     blockCount => $blockCount,
                                     offset     => $offset + $blockCount);
    my $task = Permabit::VDOTask::SliceOperation->new($slice, "write",
                                                      tag => $tag);
    $self->getAsyncTasks()->addTask($task);
  }

  # Now write the second copy of each dataset in parallel.  This should thrash
  # the dedupe index enough to generate some dedupe timeouts.
  my $beforeStats = $device->getVDOStats();
  $self->getAsyncTasks()->finish();
  my $afterStats = $device->getVDOStats();

  # We want to know that we had a dedupe timeout
  assertLTNumeric($beforeStats->{"dedupe advice timeouts"},
                  $afterStats->{"dedupe advice timeouts"});
}

1;
