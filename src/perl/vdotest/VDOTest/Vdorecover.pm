##
# Make sure that one can run Vdorecover successfully on a VDO.
#
# $Id$
##
package VDOTest::Vdorecover;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertMinMaxArgs
  assertNumArgs
);
use Permabit::Constants;
use Permabit::GenDataFiles qw(genDataFiles);
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $DEFAULT_TMP_STORAGE_SIZE = 5 * $GB / $KB;
my $INTERVAL = 10;

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks to write
     blockCount => 16000,
     # @ple Use a LVM vdo device, though a managed device should be usable too
     deviceType => 'lvmvdo',
     # @ple Use a tiny VDO to fill it easily.
     physicalSize => 750 * $MB,
     # @ple Use small slabs
     slabBits => $SLAB_BITS_TINY,
    );
##

#############################################################################
# @inherit
##
sub listSharedFiles {
  my ($self) = assertNumArgs(1, @_);
  return ($self->SUPER::listSharedFiles(),
          "src/tools/vdorecover/vdorecover",
         );
}

#############################################################################
# Run vdorecover on the specified VDO device.
##
sub runVdorecover {
  my ($self, $vdoDeviceName, $tmpStorageKB)
    = assertMinMaxArgs([$DEFAULT_TMP_STORAGE_SIZE], 2, 3, @_);
  my $machine    = $self->getDevice()->getMachine();
  my $vdorecover = $self->findBinary("vdorecover");

  # Write 'y\n' to the script every 10 seconds, a maximum of 20 times -- this
  # is chosen to be twice as many times as ever observed to be needed. We need
  # some mechanism to keep issuing 'y' to the script every so often until it
  # is done, and just piping 'yes' into it results in OOMing, so this is the
  # current, suboptimal, method to do so... 
  #
  # If the script doesn't finish within 200 seconds, it will never get more
  # input and the test will hang. 
  my $inputter
    = "i=0; while [ \$((++i)) -le 20 ]; do echo y; sleep $INTERVAL; done";
  # Have to set PATH because the script expects to find vdostats in it.
  $machine->assertExecuteCommand("$inputter | sudo LOOPBACK_DIR=/u1"
				 . " TMPFILESZ=$tmpStorageKB"
				 . " PATH=$self->{binaryDir}:\$PATH"
				 . " $vdorecover $vdoDeviceName");
}

#############################################################################
# Get the VDO device's actual path.
##
sub _getVDODevicePath {
  my ($self) = assertNumArgs(1, @_);
  my $device        = $self->getDevice();
  my $deviceName    = $device->getDeviceName();

  # If LVM creates a vdo with the nominal name of 'vdo0' in 'vg1', the visible
  # device for a filesystem is named more like 'vg1-vdo0', and the actual VDO
  # device is named vg1-vdo0pool-vpool, for LVM reasons...
  if ($device->isa("Permabit::BlockDevice::VDO::LVMManaged")) {
    my $vgName = $device->{volumeGroup}->getName();
    $deviceName = "${vgName}-${deviceName}pool-vpool";
  }

  return "/dev/mapper/$deviceName";
}

#############################################################################
# Create a VDO, put some data in (but nowhere near full), and make sure that
# vdorecover runs successfully and doesn't break anything.
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);

  my $slice = $self->createSlice(blockCount => $self->{blockCount});
  $slice->write(tag => "recov", dedupe => 0.9, direct => 1);

  my $vdoDeviceName = $self->_getVDODevicePath();
  $self->runVdorecover($vdoDeviceName);
  $slice->verify();
}

########################################################################
# Test that vdorecover works on full lvmvdo devices without a filesystem.
##
sub testFullDirect {
  my ($self) = assertNumArgs(1, @_);

  my $stats = $self->getDevice()->getVDOStats();
  my $vdoDataBlocks
    = $stats->{"physical blocks"} - $stats->{"overhead blocks used"};
  my $remainderBlockCount = $vdoDataBlocks - $self->{blockCount};

  my $slice = $self->createSlice(blockCount => $self->{blockCount});
  my $slice2 = $self->createSlice(blockCount => $remainderBlockCount,
				  offset => $self->{blockCount});
  $slice->write(tag => "recov", direct => 1);
  $slice2->writeENOSPC(tag => "recov2", direct => 1);

  my $doVdoRecover = sub {
    my $vdoDeviceName = $self->_getVDODevicePath();
    $self->runVdorecover($vdoDeviceName);
  };
  my $recoverTask = Permabit::AsyncSub->new(code => $doVdoRecover);
  $recoverTask->start();

  # Sleep to give the script time to start up.
  sleep(20);

  # Trim some data
  $slice2->trim();

  # Make sure we succeeded at recovering.
  $recoverTask->result();
  $slice->verify();
  # Sleep to make sure the input script notices that its output pipe is
  # closed and therefore stops.
  sleep($INTERVAL);
}

########################################################################
# Sync the device data out of the page cache.  The fsync command may return
# with a failure because writing a block to a full VDO has an error, so
# just repeat the fsync until it completes without error.
##
sub syncDeviceIgnoringErrors {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  # If we get repeated errors, the fsync may never complete (see VDO-4960).
  my $retries = 0;
  while ($retries++ < 10) {
    eval {
      $device->getMachine()->fsync($device->getSymbolicPath());
    };
    if (! $EVAL_ERROR) {
      return;
    }
  }
  die($EVAL_ERROR);
}

#############################################################################
# Test that vdorecover does the right thing when dealing with a VDO filled by
# a filesystem.
##
sub propertiesFullFilesystem {
  return  ( useFilesystem => 1 );
}

sub testFullFilesystem {
  my ($self) = assertNumArgs(1, @_);

  my $fs = $self->getFileSystem();

  # Figure out how much space to do in each dataset.
  my $stats = $self->getDevice()->getVDOStats();
  my $vdoDataBlocks
    = $stats->{"physical blocks"} - $stats->{"overhead blocks used"};
  my $remainderBlockCount = $vdoDataBlocks - $self->{blockCount};

  # This first write should succeed.
  my $dir1
    = genDataFiles(
                   async    => 1,
                   fs       => $fs,
                   fsync    => 1,
                   numBytes => $self->{blockCount} * $DEFAULT_BLOCK_SIZE,
                   numFiles => 100,
                  );
  $dir1->pollUntilDone();

  # This write, absorbing all the remaining space and then some, should fail.
  # But it may fail only when the page cache tries to write.
  my $dir2
    = genDataFiles(
                   async    => 1,
                   fs       => $fs,
                   numBytes => $remainderBlockCount * $DEFAULT_BLOCK_SIZE,
                   numFiles => 100,
                  );
  $dir2->pollUntilDone();

  $self->syncDeviceIgnoringErrors();

  # The error syncing matches either No space left on device or Input/output
  # error, but distinguishing them is hard.

  my $doVdoRecover = sub {
    my $vdoDeviceName = $self->_getVDODevicePath();
    $self->runVdorecover($vdoDeviceName);
  };
  my $recoverTask = Permabit::AsyncSub->new(code => $doVdoRecover);
  $recoverTask->start();

  # Sleep to give the script time to start up.
  sleep(20);

  # Remount the filesystem to clear it's errors.
  $fs->unmount();
  $fs->mount();

  # Delete some data and do a fstrim.
  $dir2->rm(async => 0);
  my $machine = $self->getDevice()->getMachine();
  $machine->assertExecuteCommand("sudo fstrim " . $fs->getMountDir());
  $self->syncDeviceIgnoringErrors();

  # Make sure we succeeded at recovering.
  $recoverTask->result();
  $dir1->verify();
  # Sleep to make sure the input script notices that its output pipe is
  # closed and therefore stops.
  sleep($INTERVAL);
}

1;
