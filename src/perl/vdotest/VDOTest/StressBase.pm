##
# Base class for VDO stress tests.
#
# Important test parameters when using Permabit::StressBase
#
# stressFsync - If true, the test will use the fsync option when writing data
#               sets.  If false, no fsync.
#
# stressParallel - If true, the test will use the async option when doing
#                  dataset operations using Permabit::GenDataFiles.  If false,
#                  no async.
#
# stressTimeout - How long the test should run.  Defaults to 12 hours.
#
# $Id$
##
package VDOTest::StressBase;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use List::Util qw(min sum);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::Future::Timer;
use Permabit::GenDataFiles qw(genDataFiles);
use Permabit::Utils qw(getRandomElement reallySleep selectFromWeightedMap);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Blocks per dataset plus some overhead, set by the test
     blocksPerDataset  => undef,
     # @ple what class of machine to run the test on
     clientClass       => "VDO-PMI",
     # @ple The datasets
     datasets          => [],
     # @ple The target number of datasets
     datasetCount      => 16,
     # @ple Use a VDOManager device
     deviceType        => "lvmvdo",
     # @ple The filesystem type (only xfs and ext4 are usable for this test)
     fsType            => "ext4",
     # @ple How many concurrent operations can be performed
     maxOperations     => 10,
     # @ple The type of stress test (true -> use fsync, false -> no fsync)
     stressFsync       => 1,
     # @ple The type of stress test (true -> parallel, false -> serial)
     stressParallel    => 0,
     # @ple A timeout for the test
     stressTimeout     => 12 * $HOUR,
     # @ple Use a filesystem
     useFilesystem     => 1,
     # @ple If positive, the number of VDO logical blocks that will not be used
     #      by the filesystem.  If negative, the filesystem is bigger than than
     #      the VDO logical device.
     _fsUnusableBlocks => undef,
     # @ple The number of blocks the empty filesystem reports its size to be
     _fsUsableBlocks   => undef,
    );
##

#############################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  $self->tearDownDatasets();
  $self->SUPER::tear_down();
}

#############################################################################
# In a large loop, do many operations.
##
sub testStress {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $fs     = $self->getFileSystem();

  # Set up a timer future so that every 12 hours we renew the RSVP reservations
  # for another 24 hours.
  my $renewReservations
    = Permabit::Future::Timer->new(code         => sub {
                                     my $time = int(time()) + $DAY;
                                     $self->getRSVPer()->renewAll($time);
                                   },
                                   timeInterval => 12 * $HOUR);

  # The filesystem has some directory/inode/metadata overhead, so leave 1%
  # headroom.
  $self->{_fsUsableBlocks} = int($fs->getFreeBlocks($self->{blockSize}) * .99);

  my $initialFree;
  my $stats = $device->getVDOStats();
  if (defined($stats)) {
    # We are running with a VDO device, so use both the filesystem stats and
    # the VDO device stats.
    my $fsUnusableBlocks
      = $stats->{"logical blocks"} - $self->{_fsUsableBlocks};
    $initialFree = min($stats->getFreeBlocks() - $fsUnusableBlocks,
                       $self->{_fsUsableBlocks});
    $self->{_stressVDO} = 1;
    $self->{_fsUnusableBlocks} = $fsUnusableBlocks;
  } else {
    # We are not running with a VDO device.  Assume the filesystem stats are
    # correct.
    $initialFree = $self->{_fsUsableBlocks};
    $self->{_stressVDO} = 0;
  }

  $self->{blocksPerDataset} = int($initialFree / $self->{datasetCount});
  $self->{bytesPerDataset}
    = 0.8 * $self->{blocksPerDataset} * $self->{blockSize};

  # Test loop
  my $iteration = 0;
  my $stopTime = time() + parseDuration($self->{stressTimeout});
  while (time() <= $stopTime) {
    $iteration++;
    my $datasets = scalar(@{$self->{datasets}});
    $log->info("Iteration $iteration: $datasets datasets");
    $self->operate($self->getTable());
    $self->waitBetweenIterations();
    $self->limitOperations();
    if ($self->{stressParallel}) {
      map { $_->poll() } @{$self->{datasets}};
      $self->logFutures();
    }
    $self->{datasets} = [ grep { !$_->isDeleted() } @{$self->{datasets}} ];
    $renewReservations->poll();
  }

  $self->doQuiesce();
  $self->checkFinalStressTestState();
  $self->doVerifyAll();
  $self->doQuiesce();
}

#############################################################################
# Prepare for the final verify all and tear_down.
##
sub checkFinalStressTestState {
  my ($self) = assertNumArgs(1, @_);
}

#############################################################################
# Get the number of available blocks.
#
# @return the number of available blocks.
##
sub getAvailableBlocks {
  my ($self) = assertNumArgs(1, @_);

  my $numDatasets = scalar(@{$self->{datasets}});
  my $fsAvailableBlocks
    = $self->{_fsUsableBlocks} - $numDatasets * $self->{blocksPerDataset};
  if (!$self->{_stressVDO}) {
    return $fsAvailableBlocks;
  }

  my $writingDatasetSpace = 0;
  if ($self->{stressParallel}) {
    my $numWriting = scalar(grep { $_->isWriting() } @{$self->{datasets}});
    $writingDatasetSpace = $self->{blocksPerDataset} * $numWriting;
  }

  my $vdoFreeBlocks
    = $self->getDevice()->getCurrentVDOStats()->getFreeBlocks();
  my $availablePhysicalBlocks
    = $vdoFreeBlocks - $self->{_fsUnusableBlocks} - $writingDatasetSpace;
  return min($availablePhysicalBlocks, $fsAvailableBlocks);
}

#############################################################################
# Get the table for this round of operations. Must be overriden by subclass.
#
# @return a weighted map of operation names.
##
sub getTable {
  my ($self) = assertNumArgs(1, @_);
  croak("Must be overridden");
}

#############################################################################
# Log the future actions we are waiting for
##
sub logFutures {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{stressParallel}) {
    my @whatForList = ();
    my $logFunc = sub {
      my $f = shift;
      push(@whatForList, $f->getWhatFor());
    };
    map { $_->apply($logFunc) } @{$self->{datasets}};
    if (scalar(@whatForList) == 0) {
      $log->info("NO WORK IN PROGRESS");
    } else {
      $log->info("WORK IN PROGRESS");
      map { $log->info("  Waiting for $_") } sort(@whatForList);
    }
  }
}

#############################################################################
# Perform a random operation from a weighted table.
#
# @param table  the weighted table
##
sub operate {
  my ($self, $table) = assertNumArgs(2, @_);
  my $operation = selectFromWeightedMap($table);
  my $doOperation = "do$operation";
  $log->info("Operation $operation");
  $self->$doOperation();
}

#############################################################################
# Stop any datasets which aren't finished yet.  Note that
# VDOTest::RebuildStressBase calls this directly to handle the multiple base
# class issue with tear_down.
##
sub tearDownDatasets {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{stressParallel}) {
    foreach my $dataset (@{$self->{datasets}}) {
      $self->runTearDownStep(sub { $dataset->kill() });
    }
  }
}

#############################################################################
# Wait between iterations.
##
sub waitBetweenIterations {
  my ($self) = assertNumArgs(1, @_);
  # It does not make sense to add a delay to a serial test.
  if ($self->{stressParallel}) {
    $self->doSleep();
  }
}

#############################################################################
# Get a list of the datasets that can be read.
#
# @return listref of datasets that can be read.
##
sub getReadableDatasets {
  my ($self) = assertNumArgs(1, @_);
  return [ grep { $_->canRead() } @{$self->{datasets}} ];
}

#############################################################################
# Get a list of the datasets that can be deleted.
#
# @return listref of datasets that can be deleted.
##
sub getRemovableDatasets {
  my ($self) = assertNumArgs(1, @_);
  return [ grep { $_->canDelete() } @{$self->{datasets}} ];
}

#############################################################################
# Count the number of operations that are actively reading or writing a
# dataset.  An operation that is reading one dataset and writing another
# dataset counts double.
#
# @return the number of operations that are active.
##
sub countOperations {
  my ($self) = assertNumArgs(1, @_);
  return sum(map { $_->countOperations() } @{$self->{datasets}});
}

#############################################################################
# Limit the number of parallel tasks that we run.  Wait while there are too
# many active operations.
##
sub limitOperations {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{stressParallel}) {
    while ($self->countOperations() >= $self->{maxOperations}) {
      map { $_->poll() } @{$self->{datasets}};
      sleep(1);
    }
  }
}

#############################################################################
# Perform a "Copy" operation:  Copy a dataset using cp.
##
sub doCopy {
  my ($self) = assertNumArgs(1, @_);
  my $readable = $self->getReadableDatasets();
  if (scalar(@$readable) > 0) {
    push(@{$self->{datasets}},
         getRandomElement($readable)->cp(async => $self->{stressParallel}));
  }
}

#############################################################################
# Perform a "Generate" operation:  Generate a brand new dataset.
##
sub doGenerate {
  my ($self) = assertNumArgs(1, @_);
  push(@{$self->{datasets}},
       genDataFiles(
                    async    => $self->{stressParallel},
                    compress => rand(0.95),
                    fs       => $self->getFileSystem(),
                    fsync    => $self->{stressFsync},
                    numBytes => $self->{bytesPerDataset},
                    numFiles => 100,
                   ));
}

#############################################################################
# Perform a "Nothing" operation:  Do nothing.
##
sub doNothing {
  my ($self) = assertNumArgs(1, @_);
}

#############################################################################
# Perform a "Quiesce" operation:  Wait for all datasets to finish their
# operations.
##
sub doQuiesce {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{stressParallel}) {
    map { $_->pollUntilDone() } @{$self->{datasets}};
  }
}

#############################################################################
# Perform a "Regenerate" operation:  Generate a duplicate dataset.
##
sub doRegenerate {
  my ($self) = assertNumArgs(1, @_);
  my $readable = $self->getReadableDatasets();
  if (scalar(@$readable) > 0) {
    my $dataset = getRandomElement($readable);
    push(@{$self->{datasets}},
         $dataset->generate(async => $self->{stressParallel}));
  }
}

#############################################################################
# Perform a "Remove" operation:  Remove a dataset.
##
sub doRemove {
  my ($self) = assertNumArgs(1, @_);
  my $removable = $self->getRemovableDatasets();
  if (scalar(@$removable) > 0) {
    getRandomElement($removable)->rm(async => $self->{stressParallel});
  }
}

#############################################################################
# Perform a "Restart" operation:  Do a clean restart of just the device.
##
sub doRestart {
  my ($self) = assertNumArgs(1, @_);
  $self->doQuiesce();
  my $fs = $self->getFileSystem();
  $fs->unmount();
  $self->getDevice()->restart();
  $fs->mount($self->{readOnly});
}

#############################################################################
# Perform a "Sleep" operation:  Sleep a while.
##
sub doSleep {
  my ($self) = assertNumArgs(1, @_);
  my $sleepTime = 2 + rand(13);
  $log->info("Sleeping for $sleepTime seconds");
  reallySleep($sleepTime);
}

#############################################################################
# Perform a "Tar" operation:  Copy a dataset using tar.
##
sub doTar {
  my ($self) = assertNumArgs(1, @_);
  my $readable = $self->getReadableDatasets();
  if (scalar(@$readable) > 0) {
    push(@{$self->{datasets}},
         getRandomElement($readable)->tar(async => $self->{stressParallel}));
  }
}

#############################################################################
# Perform a "Verify" operation:  Verify a dataset.
##
sub doVerify {
  my ($self) = assertNumArgs(1, @_);
  my $readable = $self->getReadableDatasets();
  if (scalar(@$readable) > 0) {
    getRandomElement($readable)->verify(async => $self->{stressParallel});
  }
}

#############################################################################
# Perform a "VerifyAll" operation:  Verify every dataset.
##
sub doVerifyAll {
  my ($self) = assertNumArgs(1, @_);
  my $readable = $self->getReadableDatasets();
  map {
       $self->limitOperations();
       $_->verify(async => $self->{stressParallel})
      } @$readable;
}

#############################################################################
# Parse a time duration test parameter.
#
# @param rawDuration  The text of the test option.  The text is a number
#                     followed by an optional suffix character:
#                       s for seconds
#                       m for minutes
#                       h for hours
#                       d for days
#
# @return the number of seconds.
#
# TODO - If another test wants to use this method, it is easy to move it to
#        Permabit::Utils.
##
sub parseDuration {
  my ($rawDuration) = assertNumArgs(1, @_);
  if ($rawDuration =~ /^(\d+)([smhd])$/) {
    if ($2 eq "s") {
      return $1;
    } elsif ($2 eq "m") {
      return $1 * $MINUTE;
    } elsif ($2 eq "h") {
      return $1 * $HOUR;
    } elsif ($2 eq "d") {
      return $1 * $DAY;
    }
  }
  return $rawDuration;
}

1;
