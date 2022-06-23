##
# Test suspending VDO during index rebuild.
#
# $Id$
##
package VDOTest::Suspend;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest::RebuildBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Using dory give the test more control of the VDO device state.
     deviceType    => "lvmvdo-dory",
     # @ple A filesystem would be unnecessary complication here.
     useFilesystem => 0,
    );
##

#############################################################################
# Make a VDO with a full index but very few journal entries. Then force a
# rebuild.
#
# @return a log cursor just before VDO (or the machine) restarts.
##
sub makeRecoveringVDO {
  my ($self) = assertNumArgs(1, @_);
  my $device     = $self->getDevice();
  my $machine    = $device->getMachine();
  my $deviceName = $device->getDeviceName();

  # Generate some data to make the VDO recovery do some work.
  my $dataPath = $self->generateDataFile($MB, "source");
  $machine->fsync($dataPath);
  $device->ddWrite(
                   if    => $dataPath,
                   count => 1,
                   bs    => $MB,
                   oflag => "direct",
                  );

  # Fill the index with chunk names so UDS rebuild takes some time.
  $device->sendMessage("index-fill");

  # If we have hit a reportable kernel log error, just stop the test now.
  $device->checkForKernelLogErrors();

  # VDO will lose the necessary stats when restarted, so check them now.
  $device->assertPerformanceExpectations();

  # Make sure we don't wait for the index to finish rebuilding
  $device->{expectIndexer} = 0;
  return $self->causeRecovery();
}

#############################################################################
# Test resuming the volume after suspending it.
##
sub testSuspendAndResume {
  my ($self) = assertNumArgs(1, @_);

  my $preSuspendCursor = $self->makeRecoveringVDO();

  my $device     = $self->getDevice();
  my $machine    = $device->getMachine();
  my $deviceName = $device->getDeviceName();

  $device->suspend();

  # Check the logs and see that we suspended during rebuild.
  my $logText = $machine->getKernelJournalSince($preSuspendCursor);
  if ($logText !~ qr/\[\s*(\d+\.\d+)\].*dmsetup: device \'.*\' suspended/) {
    confess("VDO not suspended");
  }

  if ($logText !~ qr/\[\s*(\d+\.\d+)\].*Replaying volume from chapter(.*)/) {
    confess("UDS index replay not started");
  }

  if ($logText =~ qr/\[\s*(\d+\.\d+)\].*finished save/) {
    confess("UDS index replay finished before suspend");
  }

  $device->resume();

  $device->waitForIndex(timeout => 30 * $MINUTE);
  $logText = $machine->getKernelJournalSince($preSuspendCursor);
  if ($logText !~ qr/\[\s*(\d+\.\d+)\].*replay changed index page map/) {
    confess("VDO rebuild did not complete after suspend");
  }
}

#############################################################################
# Test closing the volume while suspended mid-rebuild.
##
sub testSuspendAndClose {
  my ($self) = assertNumArgs(1, @_);

  my $preStopCursor = $self->makeRecoveringVDO();
  my $device        = $self->getDevice();
  my $machine       = $device->getMachine();

  $device->stop();
  my $logText = $machine->getKernelJournalSince($preStopCursor);
  if ($logText !~ qr/\[\s*(\d+\.\d+)\].*Replay interrupted by/) {
    confess("UDS index replay not terminated");
  }

  # Restart the VDO so test teardown will not be unhappy.
  $device->start();
}

1;
