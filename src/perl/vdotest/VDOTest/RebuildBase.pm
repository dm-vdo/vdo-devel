##
# Base class for vdo rebuild tests
#
# $Id$
##
package VDOTest::RebuildBase;

use strict;
use warnings FATAL => qw(all);
use Carp;
use File::Basename;
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertFalse
  assertGTNumeric
  assertLENumeric
  assertMinMaxArgs
  assertNear
  assertNumArgs
  assertRegexpMatches
  assertType
);
use Permabit::Constants;
use Permabit::SystemUtils qw(assertCommand);
use Permabit::Utils qw(getRandomSeed makeFullPath timeToText);

use base qw(VDOTest);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Whether to enable vdoAudit
     auditVDO        => 1,
     # @ple Either 'lvmvdo' (to do a real reboot) or 'lvmvdo-dory'
     #      to use a dory-simulated crash.
     deviceType      => "lvmvdo-dory",
     # @ple If non-zero, randomly discard (trim) this fraction of blocks
     randomlyDiscard => 0,
     # @ple If non-zero, randomly zero this fraction of blocks
     randomlyZero    => 0,
     # @ple Whether to use a filesystem
     useFilesystem   => 1,
    );
##

########################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();
  assertType("Permabit::BlockDevice::VDO", $self->getDevice());
}

########################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  my $vdo = $self->getVDODevice();
  if (defined($vdo)) {
    my $waitForRecoveryStep = sub {
      $log->info("Waiting for recovery mode to finish.");
      $vdo->waitUntilRecoveryComplete();
    };
    $self->runTearDownStep($waitForRecoveryStep);

    # Save the metadata from the (running) device if we should save logs.
    if ($self->shouldSaveLogs()) {
      $self->runTearDownStep(sub { $vdo->dumpMetadata(); });
    };
  }

  $self->SUPER::tear_down();
}

########################################################################
# Check that rebuild took less than the permitted limits.
##
sub _assertRebuildLimits {
  my ($self) = assertNumArgs(1, @_);

  # Find the VDO in the stack, and get its immediate backing device.
  my $device = $self->getVDODevice()->getStorageDevice();
  assertDefined($device);

  my $stats = $device->getDiskStats();
  $stats->logStats("VDO backing device");

  # Assert the number of IO operations could be completed in less than 30
  # seconds at 250k IOPS.
  my $totalIO = $stats->getTotalReads() + $stats->getTotalWrites();
  my $allowed = 30 * 250000;
  $log->info("$totalIO IO operations performed during rebuild ($allowed"
             . " allowed)");

  assertLENumeric($totalIO, $allowed, "Too much IO performed during recovery");
}

########################################################################
# Do a rebuild using Dory to simulate a crash, and return a log cursor#
# from the moment where VDO restarts.
##
sub causeDoryRecovery {
  my ($self) = assertNumArgs(1, @_);
  my $doryDevice = $self->getDoryDevice();
  my $vdoDevice  = $self->getVDODevice();
  my $machine    = $vdoDevice->getMachine();

  my $doStop = sub {
    $doryDevice->stopWriting();

    if ($self->{useFilesystem}) {
      $log->info("Stop Filesystem");
      my $fs = $self->getFileSystem();
      $fs->stop();
    }

    $log->info("Stop VDO");
    $vdoDevice->stop();
    $vdoDevice->dumpMetadata("before");
  };
  $machine->withKernelLogErrorCheckDisabled($doStop, "readonly");

  $log->info("Restart Dory");
  $doryDevice->restart();

  my $prerecoveryCursor = $machine->getKernelJournalCursor();
  $log->info("Recover VDO");
  $vdoDevice->recover();
  $vdoDevice->waitForDeviceReady();
  return $prerecoveryCursor;
}

########################################################################
# Force a VDO recovery and wait for the VDO to come back into existence.
#
# @return a log cursor just before VDO (or the machine) restarts.
##
sub causeRecovery {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getVDODevice();
  my $machine = $device->getMachine();
  if (defined($self->getDoryDevice())) {
    return $self->causeDoryRecovery();
  }

  my $rebootCursor = $machine->getKernelJournalCursor();
  $self->emergencyRebootMachineForDevice($device);
  return $rebootCursor;
}

########################################################################
# Perform a recovery, by power cycling the machine using an emergency
# restart. Returns after the VDO has finished rebuilding, but does not wait
# for it to exit recovery mode.
#
# @oparam remount   Whether to remount the filesystem after reboot
#                   (defaults to true)
##
sub recoverAndRestartVDO {
  my ($self, $remount) = assertMinMaxArgs([1], 1, 2, @_);
  my $vdo     = $self->getVDODevice();
  my $machine = $vdo->getMachine();
  my $fs        = $self->getFileSystem();

  # If we have hit a reportable kernel log error, just stop the test now.
  $vdo->checkForKernelLogErrors();

  my $trackRebuildTime = !defined($self->{readOnly}) || !$self->{readOnly};

  # VDO will lose the necessary stats when restarted, so check them now.
  $vdo->assertPerformanceExpectations();
  my $prerecoveryCursor = $self->causeRecovery();

  if ($trackRebuildTime) {
    # Extract rebuild info from journal log
    my $logText = $machine->getKernelJournalSince($prerecoveryCursor);
    my $startTime;
    my $endTime;
    eval {
      if ($logText !~ qr/\[\s*(\d+\.\d+)\].*[Rr]ebuilding reference counts/) {
        confess("Failed to find rebuild start");
      }
      $startTime = $1;
      if ($logText !~ qr/\[\s*(\d+\.\d+)\].*[Rr]ebuild complete/) {
        confess("Failed to find rebuild end");
      }
      $endTime = $1;
      if ($logText =~ qr/[Rr]eplaying (\d+) recovery entries into block map/) {
        $self->{entriesReplayed} = $1;
      } else {
        $self->{entriesReplayed} = 0;
      }
    };
    my $failed_EVAL_ERROR = $EVAL_ERROR;
    if ($failed_EVAL_ERROR) {
      $log->error("Failed to get expected pattern in kernel log. Contents:\n"
                  . substr($logText, 0, 1000) . " ... "
		  . substr($logText, -1000, 1000));
      confess($failed_EVAL_ERROR);
    }

    $self->{rebuildTime} = $endTime - $startTime;
    $log->info("Rebuild took " . timeToText($self->{rebuildTime})
               . " and replayed $self->{entriesReplayed} recovery entries");
    $self->_assertRebuildLimits();
  }

  # Mount the filesystem (if there is one, and we want it remounted).
  if (defined($fs) && $remount) {
    $fs->mount();
  }
}

########################################################################
# Run genDiscard with the --fraction flag on both the block device and the
# reference data file with the same random seed, ensuring they will have
# randomly-distributed zeros/holes in the same blocks.
#
# @param dataPath         Where to write the data
# @param decimatePercent  The amount to decimate the data, as a percent
# @param zeroDevice       Whether to zero or trim the device.
##
sub _runGenDiscard {
  my ($self, $dataPath, $discardFraction, $zeroDevice) = assertNumArgs(4, @_);

  my $device      = $self->getDevice();
  my $machine     = $device->getMachine();
  my $seed        = getRandomSeed();
  my $statsBefore = $self->getVDOStats();

  # When overwriting the device with zeros, issue an fdatasync to force the
  # data out of the page cache before we get the post-zero VDOStats.
  $machine->genDiscard(
                       of       => $device->getSymbolicPath(),
                       count    => $self->{blockCount},
                       bs       => $self->{blockSize},
                       fraction => $discardFraction,
                       zero     => $zeroDevice,
                       seed     => $seed,
                       stride   => 1,
                       sync     => $zeroDevice,
                      );

  # Always overwrite the test data file with zeros since discard only works on
  # block devices, and issue an fdatasync to prevent it from losing the
  # overwrites in the emergency reboot.
  $machine->genDiscard(
                       of       => $dataPath,
                       count    => $self->{blockCount},
                       bs       => $self->{blockSize},
                       fraction => $discardFraction,
                       zero     => 1,
                       seed     => $seed,
                       stride   => 1,
                       sync     => 1,
                      );

  # Do a little sanity check on stats to make sure we're discarding or zeroing
  # roughly the order of magnitude of the data we think we are.
  my $statsAfter = $self->getVDOStats();
  my $statsDelta = $statsAfter - $statsBefore;
  if (!$zeroDevice) {
    assertNear($discardFraction * $self->{blockCount},
               $statsDelta->{"bios in discard"},
               '10%', "discard requests");
    assertNear($discardFraction * $statsBefore->{"logical blocks used"},
               -$statsDelta->{"logical blocks used"},
               '10%', "logical blocks recovered by discarding");
  }
  assertNear($discardFraction * $statsBefore->{"data blocks used"},
             -$statsDelta->{"data blocks used"},
             '10%', "data blocks recovered by discarding or zeroing");
}

########################################################################
# Write some blocks and issue a flush if necessary.  Power cycle the vdo
# and check that all data is still readable.
#
# @param dataBlocks     How many data blocks to write
# @param zoneDelta      The change in physical zones to apply at recovery
# @param recoveryCount  The number of times to do a VDO recovery.
##
sub simpleRecovery {
  my ($self, $dataBlocks, $zoneDelta, $recoveryCount) = assertNumArgs(4, @_);
  # Using a simple rebuild test precludes using a filesystem, as the test
  # uses the device directly.
  assertFalse($self->{useFilesystem});
  my $device  = $self->getDevice();
  my $machine = $device->getMachine();

  my $dataSize = $dataBlocks * $self->{blockSize};
  my $dataPath = $self->generateDataFile($dataSize, "first");
  $machine->fsync($dataPath);
  my $temp = "$dataPath-temp";

  # Write some data.
  $device->ddWrite(
                   if    => $dataPath,
                   count => $dataBlocks,
                   bs    => $self->{blockSize},
                   oflag => "direct",
                  );
  # Optionally overwrite some of the data with zero blocks.
  if ($self->{randomlyZero} > 0) {
    $self->_runGenDiscard($dataPath, $self->{randomlyZero}, 1);
  }

  # Optionally trim some of the blocks from the volume.
  if ($self->{randomlyDiscard} > 0) {
    my $vdoDevice = $self->getVDODevice();
    # XXX VDO-2959: we have seen no discards happen at all. Just in case
    # it ever happens again, double-check that when the test ran, VDO claimed
    # to be supporting discards as it usually does.
    my $sysfsPath = makeFullPath("/sys/block",
                                 basename($vdoDevice->getDevicePath()),
                                 "queue/discard_max_bytes");
    my $discardMaxBytes = $machine->catAndChomp($sysfsPath);
    $log->debug("VDO reports $discardMaxBytes bytes as the max discard size");
    assertLENumeric($self->{blockSize}, $discardMaxBytes, $sysfsPath);

    $self->_runGenDiscard($dataPath, $self->{randomlyDiscard}, 0);
  }

  # Write a zero block to issue a flush.
  $device->ddWrite(
                   if    => "/dev/zero",
                   seek  => $dataBlocks,
                   count => 1,
                   bs    => $self->{blockSize},
                   oflag => "direct",
                   conv  => "fdatasync",
                  );

  if ($zoneDelta != 0) {
    $self->{physicalThreadCount} += $zoneDelta;
    my $newSettings = { "physical_threads" => $self->{physicalThreadCount} };
    $device->{pendingSettings} = $newSettings;
  }

  # If we're not replaying journal entries on the first recovery,
  # the test probably isn't doing anything useful.
  $self->recoverAndRestartVDO();
  assertGTNumeric($self->{entriesReplayed}, $dataBlocks / 4,
                  "recovery journal entries replayed");

  # If the test requests immediately recovering again, no more recovery work
  # should happen.
  foreach my $i ( 2 .. $recoveryCount ) {
    $self->recoverAndRestartVDO();
    assertEqualNumeric($self->{entriesReplayed}, 0,
                       "recovery journal entries replayed");
  }

  # Verify the data.
  $device->ddRead(
                  of    => $temp,
                  count => $dataBlocks,
                  bs    => $self->{blockSize},
                 );
  $machine->runSystemCmd("cmp $dataPath $temp");
}

1;
