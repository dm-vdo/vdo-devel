##
# Test creating and tearing down VDO devices many times [VDO-3572].
#
# Also, take advantage of this to test a former sysfs race condition, where
# sysfs nodes were created too early and could cause crashes if read before
# startup was complete. [VDO-4155]
#
# $Id$
##
package VDOTest::Create03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertNumArgs
  assertRegexpDoesNotMatch
  assertRegexpMatches
);
use Permabit::VDOTask::ReadSysfsWhenNotRunning;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple The type of VDO device to use, default is managed
  deviceType                => "lvmvdo",
  # @ple Number of times to restart the device
  iterationCount            => 1024,
  # @ple Don't verbosely log at shutdown to avoid 100M+ logfiles
  verboseShutdownStatistics => 0,
);
##

#############################################################################
# Test creating and destroying a device many many times.
##
sub testCreate03 {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $logCursor;                # undef the first time around
  foreach my $i (0 .. $self->{iterationCount}) {
    my $sysfsTask
      = Permabit::VDOTask::ReadSysfsWhenNotRunning->new($self->getDevice(), 0);
    $sysfsTask->start();
    $device->stop();
    if (defined($logCursor)) {
      # Skipped the first time around when setup would've logged
      # "kvdo: modprobe: loaded version..."
      my $logText = $machine->getKernelJournalSince($logCursor);
      # Check that we did log some messages.
      assertRegexpMatches(qr/ kvdo[0-9]+:/, $logText);
      # Check that they all include the device ID, excluding anything logged
      # anonymously from an interrupt context (such as a latency warning).
      assertRegexpDoesNotMatch(qr/ kvdo:(?!\[SI]:)/, $logText);
    }
    $logCursor = $machine->getKernelJournalCursor();
    $sysfsTask->result();
    $sysfsTask
      = Permabit::VDOTask::ReadSysfsWhenNotRunning->new($self->getDevice(), 1);
    $sysfsTask->start();
    $device->start();
    $sysfsTask->result();
  }
}

1;
