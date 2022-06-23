##
# Test VDO rebuild behavior when the device dies unexpectedly.
#
# This test uses the "dory" device to suddenly stop the storage device from
# doing writes.  It expects the rebuild to succeed, and for a vdoAudit to
# succeed.  There are two reasonable cases to run:
#
#   DoryRebuild03::testNoCache*   - no data cache
#   DoryRebuild03::testMiniCache* - small data cache
#
# $Id$
##
package VDOTest::DoryRebuild03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::Utils qw(getRandomElement getRandomElements);
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest::DoryBase);

my $DURATION = $HOUR;

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# Return the suite with all the tests
##
sub suite {
  my ($package) = assertNumArgs(1, @_);
  my $suite = Test::Unit::TestSuite->empty_new($package);

  my $name = "${package}::testNoCache";
  my $test = $package->make_test_from_coderef(\&_testLoop, $name);
  $test->{doryOptions} = { cacheBlocks => 0, };
  $suite->add_test($test, $name);

  $name = "${package}::testMiniCache";
  $test = $package->make_test_from_coderef(\&_testLoop, $name);
  $test->{doryOptions} = { cacheBlocks => 5, };
  $suite->add_test($test, $name);
  return $suite;
}

########################################################################
##
sub _testLoop {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getDevice()->getMachine();
  my $dataArgs  = $self->_makeDataArgs();
  my $allSlices = $self->_makeSlices();

  my $startTime = time();
  while (1) {
    my @slices = getRandomElements($allSlices, 2);
    for my $slice (@slices) {
      # Write data to a slice, stopping abruptly after 30 seconds.
      $machine->changeKernelLogErrorChecks(del => ["readonly"]);
      my $task =
        Permabit::VDOTask::SliceOperation->new($slice, "writeEIO",
                                               direct => 1,
                                               %{getRandomElement($dataArgs)});
      $self->getAsyncTasks()->addTask($task);
      $task->start();
      $self->stopDoryDelayed(30);
      $task->result();
      # Try to recover
      $self->recoverAndAuditVDO();
      # Quit after the proper amount of time has elapsed
      if (time() - $startTime > $DURATION) {
        return;
      }
      $self->getDevice()->start();
    }
    # Trim the first selected slice to ensure empty space for future writes.
    $slices[0]->trim();
    $self->getDevice()->doVDOSync();
    # Quit after the proper amount of time has elapsed
    if (time() - $startTime > $DURATION) {
      return;
    }
  }
}

#############################################################################
# Make a list of all the slice data arguments
#
# @return the list of slice data arguments
##
sub _makeDataArgs {
  my ($self) = assertNumArgs(1, @_);
  my @dataArgs;
  my %dedupeTypes = (
                     "D0"  => 0,
                     "D45" => 0.45,
                     "D90" => 0.9,
                    );
  my %compressTypes = (
                       "C0"  => 0,
                       "C60" => 0.6,
                       "C90" => 0.9,
                      );
  foreach my $dedupeKey (keys(%dedupeTypes)) {
    foreach my $compressKey (keys(%compressTypes)) {
      push(@dataArgs, {
                       tag      => "$compressKey$dedupeKey",
                       compress => $compressTypes{$compressKey},
                       dedupe   => $dedupeTypes{$dedupeKey},
                      });
    }
  }
  return \@dataArgs;
}

########################################################################
# Divide the VDO device into a list of slices
##
sub _makeSlices {
  my ($self) = assertNumArgs(1, @_);
  my $vdoDevice = $self->getDevice();
  my $stats = $vdoDevice->getVDOStats();
  my $NUM_SLICES = 4;
  my $blockCount = int($stats->{"logical blocks"} / $NUM_SLICES);
  my $blockSize  = $stats->{"block size"};

  my @slices;
  foreach my $sliceNumber (0 .. ($NUM_SLICES - 1)) {
    my $offset = $sliceNumber * $blockCount;
    push(@slices, $self->createSlice(device     => $vdoDevice,
                                     blockCount => $blockCount,
                                     blockSize  => $blockSize,
                                     offset     => $offset));
  }
  return \@slices;
}

1;
