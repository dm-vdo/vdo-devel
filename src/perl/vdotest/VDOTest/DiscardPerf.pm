##
# Test discard performance
#
# $Id$
##
package VDOTest::DiscardPerf;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertLENumeric assertNumArgs);
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
##
sub testDiscard {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $hostname = $machine->getName();
  $machine->dropCaches();

  # We will use these values on non-VDO devices
  my $blockCount = $self->{logicalSize} / $self->{blockSize};
  my $blockSize  = $self->{blockSize};

  my $stats = $device->getVDOStats();
  if (defined($stats)) {
    $blockCount = $stats->{"logical blocks"};
    $blockSize  = $stats->{"block size"};
    assertEqualNumeric(0, $stats->{"data blocks used"}, "no data blocks used");
    # LVM often writes a block when setting up.
    assertLENumeric($stats->{"logical blocks used"}, 1,
                    "at most one logical block used");
  }

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
  $log->info("Discard $size on $hostname took $duration at $rate/sec");

  $stats = $device->getVDOStats();
  if (defined($stats)) {
    assertEqualNumeric(0, $stats->{"data blocks used"}, "no data blocks used");
    assertEqualNumeric(0, $stats->{"logical blocks used"},
                       "no logical blocks used");
  }
}

1;
