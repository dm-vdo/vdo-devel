##
# Test system behavior when VDO nears running out of physical space.
#
# The major points to this test are:
#
# - We will run dmeventd and let it display warnings into 
#     the appropriate log.
# - We will write to 80+%, then 5% after that and check logs
#     to make sure the warnings show at the correct times.
#
# $Id$
##
package VDOTest::FullWarnDmEventd;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEqualNumeric
  assertGENumeric
  assertLENumeric
  assertLTNumeric
  assertNumArgs
  assertRegexpMatches
);
use Permabit::CommandString::FIO;
use Permabit::Constants;
use Permabit::Utils qw(retryUntilTimeout);

use base qw(VDOTest::FullBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple use a large enough logical size so that VDO will report ENOSPC
     #      errors
     logicalSize       => 8 * $GB,
     # @ple use a small physical size for the underlying storage so that we
     #      can fill VDO quickly
     physicalSize      => 4 * $GB,
    );
##

########################################################################
##
sub testNoSpace {
  my ($self) = assertNumArgs(1, @_);

  my $device = $self->getDevice();
  my $stats = $device->getVDOStats();
  my $totalBlocks = $stats->{'physical blocks'};
  my $freeBlocks = $self->_getFreeBlocks($stats);
  my $fullFraction = ($totalBlocks - $freeBlocks) / $totalBlocks;

  $log->info("Physical Blocks: $totalBlocks");
  $log->info("Usable Blocks:   $freeBlocks");

  # Make sure the initial VDO is less than 50% full.
  assertLTNumeric($fullFraction, .50);

  # There should be no dmeventd warnings
  my @warnings = $self->_getFullnessWarnings();
  assertEqualNumeric(scalar @warnings, 0);

  # Write to thresholds and check for dmeventd messages
  $self->_writeToFullnessPercent(80);
  $self->_writeToFullnessPercent(85);
  $self->_writeToFullnessPercent(90);
  $self->_writeToFullnessPercent(95);
  $self->_writeToFullnessPercent(100);
}

########################################################################
# Get the number of blocks that can be used for data.
#
# @param stats  The stats returned from getVDOStats.
#
# @return the number of free blocks.
##
sub _getFreeBlocks {
  my ($self, $stats) = assertNumArgs(2, @_);
  return $stats->{"physical blocks"} - $stats->{"overhead blocks used"}
    - $stats->{"data blocks used"};
}

########################################################################
# Return all the dmeventd VDO full messages in syslog from the last
# set position.
##
sub _getFullnessWarnings {
  my ($self) = assertNumArgs(1, @_);

  my $machine = $self->getUserMachine();
  my @lines = split("\n", $machine->getNewJournal());
  my @matches = grep(/VDO .* is now .* full/, @lines);
  return @matches;
}

########################################################################
# Fill up the VDO till a certain percentage and make sure the dmeventd
# warning appeared.
##
sub _writeToFullnessPercent {
  my ($self, $percent) = assertNumArgs(2, @_);

  my $device = $self->getDevice();
  my $machine = $self->getUserMachine();

  my $stats = $device->getVDOStats();
  my $totalBlocks = $stats->{'physical blocks'};
  my $freeBlocks = $self->_getFreeBlocks($stats);
  my $fullFraction = ($totalBlocks - $freeBlocks) / $totalBlocks;

  # Make sure we aren't already past the threshold
  assertLTNumeric($fullFraction, $percent / 100);

  # Fill the device using direct writes just past the threshold
  my $addFraction = (($percent + 1) / 100) - $fullFraction;
  my $blockCount = int(($freeBlocks * ($addFraction / (1 - $fullFraction)))
		       + 0.5);
  my $offset = $stats->{'data blocks used'};
  my $blockSize = $stats->{'block size'};

  my $fioJobs = 4;
  my $fioOffset = $offset * $blockSize;
  my $writePerJob = int($blockCount / $fioJobs) * $blockSize;

  my $fioOptions
    = {
       cleanupBenchmark       => 0,
       directIo               => 1,
       filename               => $device->getSymbolicPath(),
       ioType                 => "write",
       jobs                   => $fioJobs,
       offset                 => $fioOffset,
       offsetIncrement        => $writePerJob,
       writePerJob            => $writePerJob,
      };
  my $fio = Permabit::CommandString::FIO->new($self, $fioOptions);

  eval {
    $device->getMachine()->assertExecuteCommand("($fio)");
  };
  if ($EVAL_ERROR) {
    assertRegexpMatches(qr(: No space left on device),
	                $device->getMachine()->getStderr());
  }

  # Wait at least 10 seconds to make sure dmeventd fired an event.
  sleep(15);

  # This has the side effect of logging the stats, which is useful for debug.
  $device->getVDOStats();

  # There should be one new fullness warning.
  my @warnings = $self->_getFullnessWarnings();
  assertEqualNumeric(scalar @warnings, 1);

  my $regexp = "VDO pool " . $device->getVDODeviceName() . " is now .* full";
  assertRegexpMatches(qr/$regexp/, $warnings[0]);

  $regexp = "VDO pool .* is now ([0-9]+\.[0-9]+)% full";
  if ($warnings[0] =~ qr/$regexp/) {
    my $percentFull = $1;
    assertGENumeric($percentFull, $percent);
    assertLTNumeric($percentFull, $percent + 5);
  } else {
    croak("Incorrect fullness string in dmeventd");
  }
}

1;
