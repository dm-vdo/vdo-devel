##
# This is a fio test with a shifting working set, to test the ability of
# the block map to quickly evict pages when they are no longer being used.
#
# $Id$
##
package VDOTest::ShiftingWorkingSet;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::AlbireoTestUtils qw(getAlbGenConfigFile);
use Permabit::Assertions qw(assertGENumeric assertNumArgs);
use Permabit::CommandString::FIO;
use Permabit::Constants;
use Permabit::FIOUtils qw(runFIO);

use base qw(VDOTest::VDOPerfBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

###############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The number of times to shift the working set
     iterations   => 10,
     # @ple The size of the logical space
     logicalSize  => 200 * $GB,
     # @ple The size of the underlying physical device (must be big enough
     #      that the entire logical space can be filled)
     physicalSize => 270 * $GB,
     # @ple The run time per iteration
     runTime      => undef,
     # @ple Warmup must be used so we know how big the block map is
     vdoWarmup    => 1,
     # @ple The size of the working set
     writePerJob  => 100 * $GB,
    );
##

###############################################################################
# Test of repeated working set shifting.
##
sub testShiftingWorkingSet {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $devicePath = $device->getDevicePath();

  # Use a configuration for 0% dedupe
  my $configFile = getAlbGenConfigFile($self->{writePerJob}, 0,
                                       $self->{blockSize}, "Initial");
  my %perIterationOptions
    = (
       jobName              => "perIterationStream",
       albGenStream         => $configFile,
       blockSize            => $self->{blockSize},
       directIo             => 1,
       cleanupBenchmark     => 0,
       filename             => $devicePath,
       ioType               => "randwrite",
       ioEngine             => "libaio",
       ioDepth              => 128,
       ioDepthBatchSubmit   => undef,
       ioDepthBatchComplete => undef,
       gtod_reduce          => 0,
       group_reporting      => 1,
       offset               => 0,
       randrepeat           => 1,
       runTime              => $self->{runTime},
       thread               => 1,
       jobs                 => 1,
       scrambleBuffers      => 0,
      );

  # We assume the only block map reads are from vdoWarmup.
  my $vdoStatsPre = $self->getDevice()->getVDOStats();
  # Each iteration will write randomly within a writePerJob window in the
  # logical space, each offset from the previous by
  # (logicalSize - writePerJob) / iterations blocks.
  my $offsetPerWindow = (($self->{logicalSize} - $self->{writePerJob})
                         / $self->{iterations});
  # Round down to a block border
  $offsetPerWindow
    = $self->{blockSize} * int($offsetPerWindow / $self->{blockSize});
  # An initial run, which must fill the entire cache from scratch.
  my $fioCommand
    = Permabit::CommandString::FIO->new($self, \%perIterationOptions);
  my $results = runFIO($self->getDevice()->getMachine(), $fioCommand);
  my $writeRate = $results->{write}->{rate} / $MB;
  $log->info("Initial write rate: $writeRate MB/s");

  my @writeRates = ();
  for my $i (1 .. $self->{iterations}) {
    $log->info("Iteration $i");
    $fioCommand->{albGenStream}
      = getAlbGenConfigFile($self->{writePerJob}, 0, $self->{blockSize},
                            "Iteration$i");
    $fioCommand->{offset} = $offsetPerWindow * $i;
    $results = runFIO($self->getDevice()->getMachine(), $fioCommand);
    $writeRate = $results->{write}->{rate} / $MB;
    $log->info("write rate: $writeRate MB/s");
    push(@writeRates, $writeRate);
  }

  my $vdoStatsPost = $self->getDevice()->getVDOStats();
  my $vdoStats     = $vdoStatsPost - $vdoStatsPre;
  assertGENumeric($vdoStats->{"block map pages loaded"},
                  $vdoStatsPre->{"block map pages loaded"},
                  "Test read entire block map at least once");
}

1;
