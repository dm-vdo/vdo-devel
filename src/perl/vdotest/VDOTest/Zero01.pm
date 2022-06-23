##
# VDO test writing blocks of zeroes
#
# $Id$
##
package VDOTest::Zero01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants;
use Permabit::Utils qw(makeFullPath sizeToText timeToText);
use Time::HiRes qw(time);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks in a data set
     blockCount => 200000,
     # @ple Use a managed device (overridden by some specific tests)
     deviceType => "lvmvdo",
     # @ple Set to false to turn off verification
     verify     => 1,
    );
##

#############################################################################
# Test on the dedupe device
##
sub testDedupe {
  my ($self) = assertNumArgs(1, @_);
  $self->zeroTest();
}

#############################################################################
# Test on the LVM device
##
sub propertiesLogical {
  my ($self) = assertNumArgs(1, @_);
  return (
          deviceType => "linear",
          lvmSize    => 10 * $GB,
         );
}

#############################################################################
# Test on the LVM device
##
sub testLogical {
  my ($self) = assertNumArgs(1, @_);
  $self->zeroTest();
}

#############################################################################
# Test on the default device
##
sub propertiesBasic {
  my ($self) = assertNumArgs(1, @_);
  return (deviceType => undef);
}

#############################################################################
# Test on the default device
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  $self->zeroTest();
}

#############################################################################
# Test using discard on the dedupe device
##
sub testDiscard {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $dataSize = $self->{blockCount} * $self->{blockSize};

  # Trim using genDiscard on the raw device
  $machine->dropCaches();
  $self->assertNoBlocksUsed($device, "before");
  my $startTime = time();
  $machine->genDiscard(
                       of    => $device->getSymbolicPath(),
                       count => $self->{blockCount},
                       bs    => $self->{blockSize},
                      );
  $self->logRate($startTime, time(), "Discard", "raw", $device);
  $self->assertNoBlocksUsed($device, "after writing");
}

#############################################################################
# Write lots of zero blocks onto a device
##
sub zeroTest {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $dataSize = $self->{blockCount} * $self->{blockSize};
  # Write using dd onto the raw device
  $machine->dropCaches();
  $self->assertNoBlocksUsed($device, "before");
  my $startTime = time();
  $device->ddWrite(
                   if    => "/dev/zero",
                   count => $self->{blockCount},
                   bs    => $self->{blockSize},
                  );
  $machine->runSystemCmd("sync; sync");
  $self->logRate($startTime, time(), "Writing", "raw", $device);
  $self->assertNoBlocksUsed($device, "after writing");

  # Read using dd from the raw device
  $machine->dropCaches();
  $startTime = time();
  $device->ddRead(
                  of    => "/dev/null",
                  count => $self->{blockCount},
                  bs    => $self->{blockSize},
                 );
  $machine->runSystemCmd("sync; sync");
  $self->logRate($startTime, time(), "Reading", "raw", $device);
  $self->assertNoBlocksUsed($device, "after reading");

  # Test that we are reading zero blocks
  my $readPath = makeFullPath($machine->{scratchDir}, "read");
  my $zeroPath = makeFullPath($machine->{scratchDir}, "zero");
  if ($self->{verify}) {
    $device->ddRead(
                    of    => $readPath,
                    count => $self->{blockCount},
                    bs    => $self->{blockSize},
                   );
    $machine->dd(
                 if    => "/dev/zero",
                 of    => $zeroPath,
                 count => $self->{blockCount},
                 bs    => $self->{blockSize},
                );
    $machine->runSystemCmd("cmp $readPath $zeroPath");
  }

  # Write using a filesystem
  $machine->dropCaches();
  my $fs = $self->createFileSystem($device);
  my $mountPoint = $fs->getMountDir();
  $startTime = time();
  $machine->dd(
               if    => "/dev/zero",
               of    => "$mountPoint/zero",
               count => $self->{blockCount},
               bs    => $self->{blockSize},
              );
  $machine->runSystemCmd("sync; sync");
  $self->logRate($startTime, time(), "Writing", "$mountPoint on", $device);

  # Read using a filesystem
  $machine->dropCaches();
  $startTime = time();
  $machine->dd(
               if    => "$mountPoint/zero",
               of    => "/dev/null",
               count => $self->{blockCount},
               bs    => $self->{blockSize},
              );
  $machine->runSystemCmd("sync; sync");
  $self->logRate($startTime, time(), "Reading", "$mountPoint on", $device);

  # Test that we are reading zero blocks
  if ($self->{verify}) {
    $machine->runSystemCmd("cmp $mountPoint/zero $zeroPath");
  }
}

#############################################################################
# Assert that if this a dedupe device, no blocks are in use
#
# @param device  the device
# @param label   a label for logging
##
sub assertNoBlocksUsed {
  my ($self, $device, $label) = assertNumArgs(3, @_);
  # If this is not a deduped device, getVDOStats will croak.  Use eval to
  # eat the error.
  my $stats = eval { return $device->getVDOStats(); };
  if (defined($stats)) {
    $stats->logStats($label);
    assertEqualNumeric(0, $stats->{"data blocks used"}, "no blocks used");
  }
}

#############################################################################
# Log time and rate to do an operation
#
# @param start    the starting time
# @param finish   the ending time
# @param doing    reading or writing
# @param devType  device type
# @param device   the Permabit::BlockDevice
##
sub logRate {
  my ($self, $start, $finish, $doing, $devType, $device)
    = assertNumArgs(6, @_);
  my $duration = $finish - $start;
  my $devPath = $device->getSymbolicPath();
  my $rate = sizeToText($self->{blockSize} * $self->{blockCount} / $duration);
  $duration = timeToText($duration);
  $log->info("$doing $self->{blockCount} zero blocks onto $devType $devPath"
             . " took $duration at $rate/sec");
}

1;
