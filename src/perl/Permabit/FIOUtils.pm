##
# Method to run a FIO command benchmark
#
# $Id$
##
package Permabit::FIOUtils;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp qw(confess);
use Data::Dumper;
use List::MoreUtils;
use Permabit::Assertions qw(
  assertEqualNumeric
  assertLENumeric
  assertNear
  assertNumArgs
  assertTrue
  assertType
);
use Permabit::Constants qw($KB);
use Permabit::LabUtils qw(getTotalRAM);

use base qw(Exporter);

our @EXPORT_OK = qw(runFIO);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

# Fsyncs can take a *long* time on a HDD.
my $FSYNC_TIME_BUDGET = 70000;

# Prototypes have to be defined before they are used.
sub pairwise (&$$); ## no critic (ProhibitSubroutinePrototypes)

###############################################################################
# Run a FIO benchmark
#
# @param machine     The Permabit::UserMachine to run on
# @param fioCommand  The Permabit::CommandString::FIO to run
#
# @return the benchmark result hash
##
sub runFIO {
  my ($machine, $fioCommand) = assertNumArgs(2, @_);
  assertType("Permabit::UserMachine", $machine);
  assertType("Permabit::CommandString::FIO", $fioCommand);

  # Make the output easier to parse.
  local $fioCommand->{group_reporting} = 1;
  local $fioCommand->{minimal}         = 1;

  # Make sure any pending blkid scans (that may have been triggered by
  # lvcreate, etc) are completed, and thus aren't keeping the page
  # cache active.
  $machine->runSystemCmd("sudo udevadm settle");

  # Create the target directory if it doesn't exist.
  if (defined($fioCommand->{directory})) {
    $machine->runSystemCmd("mkdir -p $fioCommand->{directory}");
  }

  # Run the command.
  $machine->runSystemCmd("($fioCommand)");
  my $rawResults = $machine->getStdout();
  my $results;
  eval {
    $results = _parseFIOResults($rawResults);
  };
  if ($EVAL_ERROR) {
    $log->error("Could not parse raw FIO results, dumping...");
    $log->error($rawResults);
    confess($EVAL_ERROR);
  }

  # Clean up the target directory
  if (defined($fioCommand->{directory})) {
    $machine->executeCommand("sudo rm -rf $fioCommand->{directory}");
  }

  _assertFIOSucceeded($fioCommand, $results);
  return $results;
}

###############################################################################
# Make the standard assertions that FIO performed correctly.
#
# @param fioCommand  The Permabit::CommandString::FIO that was run.
# @param results     The benchmark result hash.
##
sub _assertFIOSucceeded {
  my ($fioCommand, $results) = assertNumArgs(2, @_);

  # Use assertNear because it looks like fio might not read/write *exactly* as
  # much data as we told it to (esp. with random, async workloads).
  if ($fioCommand->{writePerJob}) {
    # Check that the test ran to completion as defined by either the runtime
    # parameter (if specified) or by the total expected I/O.
    eval {
      if (defined($fioCommand->{runTime})) {
        assertTrue(($results->{read}->{runtime} > 0)
                    || ($results->{write}->{runtime} > 0),
                    "No runtime data available");
	my $runTimeMsec = $fioCommand->{runTime} * 1000;
	my $toleranceMsec = 60000;

	if ($fioCommand->{endFsync} && !($fioCommand->{ioType} =~ m/read/) ) {
	  # Fsync's take a long time on HDDs, so if we expect to do one after
	  # fio sends its final fsync, add that time to our time budget and
	  # tolerance.
          $toleranceMsec += $FSYNC_TIME_BUDGET / 2;
	  $runTimeMsec += $FSYNC_TIME_BUDGET / 2;
	}

        if ($results->{read}->{runtime} > 0) {
          assertNear($runTimeMsec,
                     $results->{read}->{runtime},
		     $toleranceMsec,
                     "test runtime does not coincide with requested");
        } else {
          assertNear($runTimeMsec,
                     $results->{write}->{runtime},
		     $toleranceMsec,
                     "test runtime does not coincide with requested");
        }
      }
    };
    my $runtimeCheckError = $EVAL_ERROR;

    my $nLoops = $fioCommand->{loops} // 1;
    eval {
      assertNear(
        ($fioCommand->{writePerJob} * $fioCommand->{jobs} * $nLoops),
        ($results->{read}->{bytes} + $results->{write}->{bytes}),
        "2%", "data r/w bytes does not coincide with requested"
      );
    };
    my $totalDataCheckError = $EVAL_ERROR;

    # If the valid runtime check fails with an assertion, but the total-data
    # check does not fail, then there is no error.  If the total-data check
    # fails with an assertion, but the runtime check was applied and did not
    # fail, then there is no error.  Otherwise there is an error.
    #
    # Report the runtime-check failure if both checks failed.
    if ($runtimeCheckError && $totalDataCheckError) {
      confess($runtimeCheckError);
    } elsif ($totalDataCheckError && !defined($fioCommand->{runTime})) {
      confess($totalDataCheckError);
    }
  }
}

###############################################################################
# Parse fio results
#
# @param stdout  The stdout from the fio command
#
# @return  The results hashref
##
sub _parseFIOResults {
  my ($stdout) = assertNumArgs(1, @_);

  my @lines = split(/\n/, $stdout);
  assertEqualNumeric(1, scalar(@lines), "unexpected amount of output");

  $log->debug("parsing stats for $lines[0]");
  my @ret = split(/;/, $lines[0]);
  assertEqualNumeric(3, $ret[0], "unsupported output format version");

  # There can me more entries depending on the disks that are used,
  # if continue_on_error is set, or if there is a description set.
  assertLENumeric(121, scalar(@ret), "unexpected number of values");

  # XXX Should we strip the '%' from some of the values?
  my %results = (
                 # output format version $ret[0]
                 "fio version"             => $ret[1],
                 "job name"                => $ret[2],
                 "group id"                => $ret[3],
                 "error"                   => $ret[4],
                 "read" => {
                            "bytes"        => $ret[5] * $KB,
                            "rate"         => $ret[6] * $KB,
                            "iops"         => $ret[7],
                            "runtime"      => $ret[8],
                            (pairwise {"slat $a" => $b}
                                       [qw(min max mean std)],
                                       [@ret[9..12]]),
                            (pairwise {"clat $a" => $b}
                                       [qw(min max mean std)],
                                       [@ret[13..16]]),
                            "clat percentiles" => [@ret[17..36]],
                            (pairwise {"latency $a" => $b}
                                       [qw(min max mean std)],
                                       [@ret[37..40]]),
                            (pairwise {"rate $a" => $b}
                                       [qw(min max aggrb mean std)],
                                       [@ret[41..45]]),
                           },
                 "write" => {
                             "bytes"       => $ret[46] * $KB,
                             "rate"        => $ret[47] * $KB,
                             "iops"        => $ret[48],
                             "runtime"     => $ret[49],
                             (pairwise {"slat $a" => $b}
                                        [qw(min max mean std)],
                                        [@ret[50..53]]),
                             (pairwise {"clat $a" => $b}
                                        [qw(min max mean std)],
                                        [@ret[54..57]]),
                             "clat percentiles" => [@ret[58..77]],
                             (pairwise {"latency $a" => $b}
                                        [qw(min max mean std)],
                                        [@ret[78..81]]),
                             (pairwise {"rate $a" => $b}
                                        [qw(min max aggrb mean std)],
                                        [@ret[82..86]]),
                            },
                 "cpu" => {
                           "user"                => $ret[87],
                           "system"              => $ret[88],
                           "context switches"    => $ret[89],
                           "major page faults"   => $ret[90],
                           "minor page faults"   => $ret[91],
                          },
                                              # <=1, 2, 4, 8, 16, 32, >=64
                 "io depth distribution"   => [@ret[92..98]],
                 "io latency distribution"
                   => {
                       # <=2, 4, 10, 20, 50, 100, 250, 500, 750, 1000
                       microseconds => [@ret[99..108]],
                       # <=2, 4, 10, 20, 50, 100, 250, 500, 750, 1000, 2000, >=2000
                       milliseconds => [@ret[109..120]]
                      },
                );

  # If FIO wrote to a disk, there will be a disk utilization section
  # which includes 9 items (per disk... including layered block devs)
  if (@ret >= 130) {
    $results{'disk stats'} = _getFIODiskStats([@ret[121..$#ret]]);
  }

  my $dumper = Data::Dumper->new([\%results]);
  $dumper->Purity(0)->Indent(2)->Sortkeys(1);
  $log->debug($dumper->Dump());

  return \%results;
}

###############################################################################
# Extracts the disk utilization stats from fio. For layered block devices,
# we extract the stats for each layer. The structure is as follows:
#   'disk stats' => {
#                     device => name,
#                     read   => { ios    => n,
#                                 merges => n,
#                                 ticks  => n },
#                     write  => { ios    => n,
#                                 merges => n,
#                                 ticks  => n },
#                     'time spent in queue'  => t,
#                     'disk utilization pct' => p,
#                     'child stats' => ...
#                   }
#
# @param args      The extra disk utilization stats
#
# @return the hashref of stats
##
sub _getFIODiskStats {
  my ($args) = assertNumArgs(1, @_);
  my @allStats;
  while (@$args >= 9) {
    my %diskStats;
    $diskStats{device}                 = shift(@$args);
    $diskStats{read}->{ios}            = shift(@$args);
    $diskStats{write}->{ios}           = shift(@$args);
    $diskStats{read}->{merges}         = shift(@$args);
    $diskStats{write}->{merges}        = shift(@$args);
    $diskStats{read}->{ticks}          = shift(@$args);
    $diskStats{write}->{ticks}         = shift(@$args);
    $diskStats{'time spent in queue'}  = shift(@$args);
    $diskStats{'disk utilization pct'} = shift(@$args);
    push(@allStats, \%diskStats);
  }
  return \@allStats;
}

###############################################################################
# Ugh, wrapper around List::MoreUtils::pairwise that uses arrayrefs instead
# of arrays so that the function can be more useful.
##
sub pairwise (&$$) { ## no critic (ProhibitSubroutinePrototypes)
  my ($op, $list1, $list2) = @_;
  return List::MoreUtils::pairwise { $op->() } @$list1, @$list2;
}

1;
