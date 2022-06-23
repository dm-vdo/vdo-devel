##
# Measure VDO discard performance at different settings of max_discard_sectors.
#
# It is only sensible to run this test using a VDO device, as it varies the
# setting of the vdoMaxDiscardSectors test parameter.  This parameter is only
# used when setting up VDO.
#
# It is only sensible to run this test on a Linux version newer than 4.18, as
# older kernels didn't do discard splitting.
#
# $Id$
##
package VDOTest::DiscardMaximum;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertLENumeric
  assertNumArgs
);
use Permabit::Utils qw(sizeToText timeToText);
use Time::HiRes qw(time);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple what class of machine to run the test on
     clientClass => "VDO-PMI",
     # @ple set up a VDO for the test
     deviceType  => "lvmvdo",
     # @ple logical (provisioned) size of the exported VDO device
     logicalSize => "100G",
    );
##

#############################################################################
# Make a test to add to the suite
#
# @param size       The value of vdoMaxDiscardSectors
# @param sizeName   Size name to put into the test name
##
sub _makeTest {
  my ($package, $size, $sizeName) = assertNumArgs(3, @_);
  my $name = "${package}::test_${sizeName}";
  my $test = $package->make_test_from_coderef(\&_discardTest, $name);
  $test->{vdoMaxDiscardSectors} = $size;
  return $test;
}

#############################################################################
# Return the suite with all the tests
##
sub suite {
  my ($package) = assertNumArgs(1, @_);
  my $suite = Test::Unit::TestSuite->empty_new($package);
  # Powers of two from 8 to 1K
  foreach my $size (map { 1 << $_ } (3..10)) {
    $suite->add_test($package->_makeTest($size, $size));
  }
  # XXX - It may seem reasonable to test larger powers of two, but when the
  #       size is 2K and the device is a hard drive, VDO reports a write
  #       latency of 31 seconds.
  #
  # XXX - It may seem reasonable to test larger powers of two, but when the
  #       size is 16K and the device is a hard drive, VDO reports 120 second
  #       stalls.
  #
  # XXX - It may seem reasonable to test larger powers of two, but when the
  #       size is 16K and the device is a hard drive, ISCSI fails with an EIO
  #       error caused by a 60 second timeout.
  #
  # XXX - If we want to test unlimited size discards, add this call:
  #       $suite->add_test($package->_makeTest("4294967295", "unlimited")); At
  #       this time (3-Apr-2015), unlimited is significantly slow and therefore
  #       does not need to be be run regularly.  When the size is unlimited,
  #       all three of the problems noted above will happen.
  return $suite;
}

#############################################################################
# Test using discard on the dedupe device
##
sub _discardTest {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $hostname = $machine->getName();
  $machine->dropCaches();

  my $stats = $device->getVDOStats();
  assertDefined($stats, "Test must be run on a device that uses VDO");
  assertEqualNumeric(0, $stats->{"data blocks used"}, "no data blocks used");
  # LVM often writes a block when setting up.
  assertLENumeric($stats->{"logical blocks used"}, 1,
                  "at most one logical block used");
  my $blockCount = $stats->{"logical blocks"};
  my $blockSize  = $stats->{"block size"};

  $machine->waitForDiskSelfTests();

  my $startTime = time();
  $machine->genDiscard(
                       of    => $device->getVDOSymbolicPath(),
                       count => $blockCount,
                       bs    => $blockSize,
                      );
  my $duration = time() - $startTime;

  my $size = sizeToText($blockSize * $blockCount);
  my $rate = sizeToText($blockSize * $blockCount / $duration);
  $duration = timeToText($duration);
  $log->info("Discard $size with $self->{vdoMaxDiscardSectors} maximum discard"
             . " sectors on $hostname took $duration at $rate/sec");

  $stats = $device->getVDOStats();
  assertDefined($stats, "Test must be run on a device that uses VDO");
  assertEqualNumeric(0, $stats->{"data blocks used"}, "no data blocks used");
  assertEqualNumeric(0, $stats->{"logical blocks used"},
                     "no logical blocks used");
}

1;
