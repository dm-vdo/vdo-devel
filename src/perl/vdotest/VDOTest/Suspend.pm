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
  my $vdoDevice  = $self->getDevice();
  my $doryDevice = $self->getDoryDevice();
  my $machine    = $vdoDevice->getMachine();
  my $config     = $vdoDevice->dumpConfig();

  # Add some data so that the VDO device is not empty.
  my $dataPath = $self->generateDataFile($MB, "source");
  $machine->fsync($dataPath);
  $vdoDevice->ddWrite(
                      if    => $dataPath,
                      count => 1,
                      bs    => $MB,
                      oflag => "direct",
                     );

  # If we have hit a reportable kernel log error, just stop the test now.
  $vdoDevice->checkForKernelLogErrors();

  # VDO will lose the necessary stats when restarted, so check them now.
  $vdoDevice->assertPerformanceExpectations();

  # Make sure we don't wait for the index to finish rebuilding
  $vdoDevice->{expectIndexer} = 0;

  my $doStop = sub {
    $doryDevice->stopWriting();
    $log->info("Stop VDO");
    $vdoDevice->stop();
    $vdoDevice->dumpMetadata("before");
  };
  $machine->withKernelLogErrorCheckDisabled($doStop, "readonly");

  $log->info("Restart Dory");
  $doryDevice->restart();

  # Fill the index with chunk names so UDS rebuild takes some time.
  $vdoDevice->fillIndex($config, 1);

  my $prerecoveryCursor = $machine->getKernelJournalCursor();
  $log->info("Recover VDO");
  $vdoDevice->recover();
  $vdoDevice->waitForDeviceReady();
  return $prerecoveryCursor;
}

#############################################################################
# Test resuming the volume after suspending it.
##
sub testSuspendAndResume {
  my ($self) = assertNumArgs(1, @_);

  my $preSuspendCursor = $self->makeRecoveringVDO();

  my $device     = $self->getDevice();
  my $machine    = $device->getMachine();

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
