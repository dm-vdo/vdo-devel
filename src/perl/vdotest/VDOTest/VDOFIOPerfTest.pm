##
# Class for testing kvdo performance via fio
#
# $Id$
##
package VDOTest::VDOFIOPerfTest;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNear assertNumArgs);
use Permabit::CommandString::FIO;
use Permabit::Constants;
use Permabit::FIOUtils qw(runFIO);
use Time::HiRes qw(gettimeofday);

use base qw(VDOTest::VDOFIOPerfTestBase);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

# TODO: * Which ioengine should we use? We've found libaio to only be
#         fastest for randread and randrw and psync/sync to be fastest
#         for all other workloads (even randwrite) as long as we are
#         utilizing the page cache.
#       * Report numbers in IOPs
#       * Run rand tests on a pre-allocated kvdo (otherwise due to the
#         nature of vdo, the tests just end up being seq). However, be
#         careful when using ext3 and the fio default of posix_fallocate
#         in the filesystem tests, because that will end up writing a
#         bunch of zeros which will undo the pre-allocation. Also mkfs
#         will send a bunch of BLKDISCRD ioctls when it formats the drive
#         if the -K option is not used.
#       * There should be a check that we got the expected dedupe amount

###############################################################################
##
sub testFioBenchmark {
  my ($self)              = assertNumArgs(1, @_);
  my $vdoStatsAtBeginning = $self->getVDOStats();
  my $device              = $self->getDevice();

  # We can cache this, since there are no reboots during this test, and it's
  # mostly only used in log messages anyway. It'll give us a little less
  # clutter in the logs.
  my $devicePath = $device->getSymbolicPath();
  my $machine = ($self->{useFilesystem}
                 ? $self->getFileSystem()->getMachine()
                 : $device->getMachine());

  $self->preWriteData(0);
  
  my $benchOpts = $self->extractFioBenchmarkOptions();
  my $fioCommand = Permabit::CommandString::FIO->new($self, $benchOpts);

  my $diskStatsPre  = $device->getDiskStats();
  my $vdoStatsPre   = $self->getVDOStats();
  my $startTime     = gettimeofday();
  my $results       = runFIO($machine, $fioCommand);
  my $diskStatsPost = $device->getDiskStats();
  my $diskStats     = $diskStatsPost - $diskStatsPre;
  $diskStats->logStats($devicePath);

  my $vdoStats = $self->getVDOStats();

  # Since we can only report a single throughput value per test to the grapher,
  # just sum both read and write values to get a "total" throughput and IOs.
  my $wrate = $results->{write}->{rate};
  my $wio   = $results->{write}->{bytes};
  my $rrate = $results->{read}->{rate};
  my $rio   = $results->{read}->{bytes};
  my $rate  = $wrate + $rrate;
  my $io    = $wio   + $rio;

  $log->info("write rate = " . ($wrate/$MB) . " MB/s i/o = "
                             . ($wio/$MB)   . " MB");
  $log->info("read rate = "  . ($rrate/$MB) . " MB/s i/o = "
                             . ($rio/$MB)   . " MB");
  $log->info("total rate = " . ($rate/$MB)  . " MB/s i/o = "
                             . ($io/$MB)    . " MB");

  if (defined($vdoStats)) {
    my $vdoStatsDelta = $vdoStats - $vdoStatsPre;

    $log->info("VDO data blocks used = $vdoStatsDelta->{'data blocks used'}");
    $log->info("VDO data in write = $vdoStatsDelta->{'bios in write'}");
    if ($vdoStatsDelta->{"bios in write"} == 0) {
      $log->info("VDO data write = 0, possible read test. Skip dedupe check.");
    } else {
      # We check for deduplication in the cumulative request counters instead
      # of looking at the saving percent VDO reports, since the saving
      # percent reports both dedupe and compression. Also, fio overwrites
      # blocks randomly, causing the final dedupe percent to be significantly
      # smaller than the dataset's dedupe percent.
      #
      # Overwrites can cause dedupe advice to point to blocks that have been
      # overwritten with new data. Since we're sanity-checking the fio
      # output, not our ability to preserve unreferenced blocks for
      # future deduplication, we should include the stale-advice
      # count. This is safe since there should have been nothing in
      # Albireo at the start of the test that could generate
      # additional reports of stale advice.
      my $dedupeAdviceFound = ($vdoStatsDelta->{"dedupe advice valid"}
                               + $vdoStatsDelta->{"dedupe advice stale"}
                               + $vdoStatsDelta->{"concurrent data matches"});
      my $dedupePercent = ($dedupeAdviceFound
                           / $vdoStatsDelta->{"bios in write"}) * 100;
      $log->info("Dedupe Percentage: " .  sprintf("%.2f", $dedupePercent));
      if (defined($self->{dedupePercent})) {
        my $expectedDedupe = $self->{dedupePercent};
        assertNear($expectedDedupe, $dedupePercent, 1,
                   "VDO dedupe did not match expected");
      }
    }

    $vdoStatsDelta->logStats("$devicePath - changes during test run");
    $vdoStatsDelta->logDerivedStats($devicePath);

    # Overhead blocks must not have changed during the test: if they did,
    # vdoInitializeBlockMap is not doing its job.
    if ($self->{vdoWarmup}) {
      assertEqualNumeric($vdoStatsAtBeginning->{"overhead blocks used"},
                         $vdoStats->{"overhead blocks used"},
                         "Overhead blocks used must not change if the device"
                         . " is warmed up");
    }
  }
}

1;
