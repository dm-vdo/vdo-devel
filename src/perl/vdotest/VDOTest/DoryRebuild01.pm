##
# Test VDO rebuild behavior when the device dies unexpectedly.
#
# This test uses the "dory" device without a data cache to suddenly stop the
# storage device from doing writes.  It expects the rebuild to succeed, and for
# the data to be either the "old" data (zeroes) or the "new" data that we were
# trying to write.
#
# $Id$
##
package VDOTest::DoryRebuild01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::Utils qw(timeToText);
use Permabit::VDOTask::SliceOperation;
use Time::HiRes qw(time);

use base qw(VDOTest::DoryBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Compression settings to try
     compressFractions => [0, 0.45, 0.9],
     # @ple Dedupe settings to try
     dedupeFractions   => [0, 0.6, 0.9],
     # @ple Dory device options.
     doryOptions       => { cacheBlocks => 0, },
     # @ple Time to write each dataset before a rebuild, in seconds
     timePerSlice      => 30,
    );
##

########################################################################
##
sub testForgetful {
  my ($self) = assertNumArgs(1, @_);
  my $doryDevice = $self->getDoryDevice();
  my $vdoDevice  = $self->getDevice();
  my $vdoMachine = $vdoDevice->getMachine();

  # Set up a small slice to be used for timing predictions
  my $SMALL_COUNT = 4000;
  my $smallSlice = $self->createSlice(blockCount => $SMALL_COUNT,
                                      device     => $vdoDevice);

  # Loop over a variety of dedupe and compression settings
  my $counter = 0;
  foreach my $dedupe (@{$self->{dedupeFractions}}) {
    foreach my $compress (@{$self->{compressFractions}}) {
      $counter++;

      # Write a small amount of test data, so that we can get an estimate
      # of the writing speed.
      my $startTime = time();
      $smallSlice->write(tag      => "S$counter",
                         compress => $compress,
                         dedupe   => $dedupe,
                         direct   => 1,);
      my $duration = time() - $startTime;
      my $smallRate = $SMALL_COUNT / $duration;
      $log->info("Wrote $SMALL_COUNT blocks in " . timeToText($duration)
                 . " at $smallRate blocks/second");

      # Write twice the time limit's amount of data, but stop after the limit.
      my $largeCount = int($smallRate * $self->{timePerSlice} * 2);
      my $largeSlice = $self->createSlice(blockCount => $largeCount,
                                          device     => $vdoDevice,
                                          offset     => $SMALL_COUNT);
      $vdoMachine->changeKernelLogErrorChecks(del => ["readonly"]);
      my $task =
        Permabit::VDOTask::SliceOperation->new($largeSlice, "writeEIO",
                                               tag      => "M$counter",
                                               compress => $compress,
                                               dedupe   => $dedupe,
                                               direct   => 1);
      $self->getAsyncTasks()->addTask($task);
      $startTime = time();
      $task->start();
      $self->stopDoryDelayed($self->{timePerSlice});
      $task->result();
      $duration = time() - $startTime;
      $log->info("Wrote for " . timeToText($duration));

      # Restart and verify.
      $vdoDevice->stop();
      $vdoMachine->changeKernelLogErrorChecks(add => ["readonly"]);
      $doryDevice->restart();
      $vdoDevice->recover();
      $smallSlice->verify();
      $largeSlice->verify();
      $largeSlice->trim();
    }
  }
}

1;
