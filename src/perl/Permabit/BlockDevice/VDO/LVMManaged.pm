##
# Perl object that represents a VDO Volume managed by LVM
#
# $Id$
##
package Permabit::BlockDevice::VDO::LVMManaged;

use strict;
use warnings FATAL => qw(all);
use Carp qw(confess);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertNumArgs
  assertType
);
use Permabit::Constants;
use Permabit::ProcessUtils qw(delayFailures);
use Permabit::Utils qw(makeFullPath);
use Permabit::Version qw($VDO_MARKETING_VERSION);

# We're using multiple inheritance here. VDO is used to support all VDO
# test functionality like displaying stats, etc. LVM is used to get access
# to the LVM.pm volume group and some common LVM functions like activation
# and deactivation. VDO.pm will be used for any conflicting functions.
use base qw(
  Permabit::BlockDevice::VDO
  Permabit::BlockDevice::LVM
);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# This VDO device is managed by lvm, but we still want to support all
# the VDO.pm functionality.
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2, @_);
  $self->Permabit::BlockDevice::VDO::configure($arguments);
  $self->Permabit::BlockDevice::LVM::configure($arguments);

  # Get the location of the vdoformat binary for use by LVM.
  $self->{vdoformat} = $self->findBinary("vdoformat");

  # We can't just default these in PROPERTIES or INHERITED_PROPERTIES, because
  # the base class sets them to undef in its INHERITED_PROPERTIES and those are
  # the values that VDOTest uses. Hence we have to default them here.
  $self->{blockMapCacheSize} ||= 128 * $MB;
  $self->{blockMapPeriod}    ||= 16380;
  $self->{moduleVersion}     //= $self->{vdoModuleVersion};
  $self->{moduleVersion}     //= $VDO_MARKETING_VERSION;
}

########################################################################
# Make the --config string that will be used to create VDO with. This
# overrides values in lvm.conf for this invocation of VDO.
#
##
sub makeLVMConfigString {
  my ($self) = assertNumArgs(1, @_);

  # These are the values in the lvm.conf file that match our own
  # properties. Set them appropriately.
  my @values = ();

  # Set any vdo options we should.
  if (defined($self->{bioAckThreadCount})) {
    push(@values, ["vdo_ack_threads", "$self->{bioAckThreadCount}"]);
  }
  if (defined($self->{bioThreadCount})) {
    push(@values, ["vdo_bio_threads", "$self->{bioThreadCount}"]);
  }
  if (defined($self->{bioThreadRotationInterval})) {
    push(@values,
         ["vdo_bio_rotation", "$self->{bioThreadRotationInterval}"]);
  }
  if (defined($self->{cpuThreadCount})) {
    push(@values, ["vdo_cpu_threads", "$self->{cpuThreadCount}"]);
  }
  if (defined($self->{hashZoneThreadCount})) {
    push(@values, ["vdo_hash_zone_threads", "$self->{hashZoneThreadCount}"]);
  }
  if (defined($self->{logicalThreadCount})) {
    push(@values, ["vdo_logical_threads", "$self->{logicalThreadCount}"]);
  }
  if (defined($self->{physicalThreadCount})) {
    push(@values, ["vdo_physical_threads", "$self->{physicalThreadCount}"]);
  }

  if (defined($self->{enableCompression})) {
    # magic value -1 suppresses the option completely
    if ($self->{enableCompression} != -1) {
      push(@values, ["vdo_use_compression", $self->{enableCompression}]);
    }
  }
  if (defined($self->{enableDeduplication})) {
    # magic value -1 suppresses the option completely
    if ($self->{enableDeduplication} != -1) {
      push(@values, ["vdo_use_deduplication", $self->{enableDeduplication}]);
    }
  }

  if (defined($self->{blockMapCacheSize})) {
    push(@values,
         ["vdo_block_map_cache_size_mb", $self->{blockMapCacheSize} / $MB]);
  }
  if (defined($self->{blockMapPeriod})) {
    push(@values, ["vdo_block_map_period", $self->{blockMapPeriod}]);
  }
  if (defined($self->{slabBits})) {
    push(@values,
         ["vdo_slab_size_mb", (2 ** $self->{slabBits} / 256)]);
  }

  if (defined($self->{emulate512Enabled})) {
    push(@values,
         ["vdo_minimum_io_size", $self->{emulate512Enabled} ? 512 : 4096]);
  }
  if (defined($self->{vdoMaxDiscardSectors})) {
    push(@values, ["vdo_max_discard", $self->{vdoMaxDiscardSectors} / 8]);
  }

  # Set any index options we should.
  if (defined($self->{memorySize})) {
    push(@values,
         ["vdo_index_memory_size_mb", ($self->{memorySize} * $GB) / $MB]);
  }
  if (defined($self->{sparse})) {
    push(@values, ["vdo_use_sparse_index", int($self->{sparse})]);
  }

  @values = map { "allocation/" . join("=", @$_) } @values;

  if (defined($self->{vdoformat})) {
    push(@values,
         join("=", "global/vdo_format_executable", $self->{vdoformat}));
  }

  return join(" ", @values);
}

########################################################################
# @inherit
##
sub setup {
  my ($self) = assertNumArgs(1, @_);
  # This will create two devices at once; the vdo pool and the
  # volume on top of it.
  my $config = $self->makeLVMConfigString();
  my $totalSize = $self->getTotalSize($self->{physicalSize});
  $self->{volumeGroup}->createVDOVolume($self->{deviceName},
                                        "$self->{deviceName}pool",
                                        $totalSize,
                                        $self->{logicalSize},
                                        $config);
  $self->Permabit::BlockDevice::VDO::setup();
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  # Calling this will enable the device via LVM and add disableLogicalVolume
  # to the list of deactivation steps.
  $self->Permabit::BlockDevice::LVM::activate();
}

########################################################################
# @inherit
##
sub installModule {
  my ($self) = assertNumArgs(1, @_);

  assertDefined($self->{binaryDir});
  if (!defined($self->getModuleSourceDir())) {
    $self->setModuleSourceDir($self->{binaryDir});
  }

  $self->Permabit::BlockDevice::VDO::installModule();
}

########################################################################
# @inherit
##
sub growLogical {
  my ($self, $logicalSize) = assertNumArgs(2, @_);

  # resize the vdo logical volume
  my $name = "$self->{deviceName}";

  my $newSize = $self->{volumeGroup}->alignToExtent($logicalSize);
  $self->{volumeGroup}->resizeLogicalVolume($name, $newSize);
  $self->{logicalSize} = $newSize;
}

########################################################################
# @inherit
##
sub growPhysical {
  my ($self, $physicalSize) = assertNumArgs(2, @_);

  # resize the vdo pool
  my $name = "$self->{deviceName}pool";

  my $newSize = $self->{metadataSize} + $physicalSize;
  $newSize = $self->{volumeGroup}->alignToExtent($newSize);
  $self->{volumeGroup}->resizeLogicalVolume($name, $newSize);
  $self->{physicalSize} = $newSize - $self->{metadataSize};
}

########################################################################
# @inherit
##
sub getLogicalMetadataSize {
  my ($self) = assertNumArgs(1, @_);
  return $MB;
}

########################################################################
# @inherit
##
sub resizeStorageDevice {
  my ($self, $physicalSize) = assertNumArgs(2, @_);
  # LVM creates and manages its own linear backing store, so we don't
  # need to handle resizing of the storage device here, like Managed
  # and Unmanaged do.
}

########################################################################
# Rename a VDO device. Currently LVM managed devices are the only
# ones that support rename.
#
# @param newName The new name for the VDO device.
##
sub renameVDO {
  my ($self, $newName) = assertNumArgs(2, @_);
  my $name = "$self->{deviceName}";
  my $pool = "$self->{deviceName}pool";
  $self->{volumeGroup}->renameLogicalVolume($name, $newName);
  $self->{volumeGroup}->renameLogicalVolume($pool, $newName . "pool");
  $self->setDeviceName($newName);
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);
  # Make sure to complete teardown even if we encounter errors.
  my @steps
    = (
       sub {
         $self->{volumeGroup}->deleteLogicalVolume($self->getDeviceName());
       },
       sub {
         $self->Permabit::BlockDevice::VDO::teardown();
       },
      );
  delayFailures(@steps);
}

########################################################################
# @inherit
##
sub migrate {
  my ($self, $newMachine) = assertNumArgs(2, @_);
  my $currentHost = $self->getMachineName();
  my $newHost = $newMachine->getName();

  my $migrate = sub {
    if ($currentHost eq $newHost) {
      $self->installModule();
      return;
    }

    $log->info("Migrating VDO device from $currentHost to $newHost");
    $self->getStorageDevice()->migrate($newMachine);
    $self->installModule();
  };
  $self->runWhileStopped($migrate);
}

########################################################################
# @inherit
##
sub disableWritableStorage {
  my ($self) = assertNumArgs(1, @_);
  my $deviceName = "$self->{deviceName}pool_vdata";
  my $vgName   = $self->{volumeGroup}->getName();
  my $fullName = "$vgName-$deviceName";

  $self->runOnHost("sudo dmsetup remove $fullName");
}

########################################################################
# @inherit
##
sub enableWritableStorage {
  my ($self) = assertNumArgs(1, @_);
  my $deviceName = "$self->{deviceName}pool_vdata";

  $self->{volumeGroup}->enableLogicalVolume($deviceName);

  my $vgName   = $self->{volumeGroup}->getName();
  my $fullName = "$vgName-$deviceName";
  my $table = $self->runOnHost("sudo dmsetup table $fullName");

  $self->{volumeGroup}->disableLogicalVolume($deviceName);

  $self->runOnHost("sudo dmsetup create $fullName --table '$table'");
}

########################################################################
# @inherit
##
sub disableReadableStorage {
  my ($self) = assertNumArgs(1, @_);
  $self->{volumeGroup}->disableLogicalVolume("$self->{deviceName}pool_vdata");
}

########################################################################
# @inherit
##
sub enableReadableStorage {
  my ($self) = assertNumArgs(1, @_);
  $self->{volumeGroup}->enableLogicalVolume("$self->{deviceName}pool_vdata");
}

########################################################################
# @inherit
##
sub getVDOStoragePath {
  my ($self) = assertNumArgs(1, @_);
  if (defined($self->{volumeGroup})) {
    my $vgName      = $self->{volumeGroup}->getName();
    my $storageName = "$vgName-$self->{deviceName}pool_vdata";
    my $path        = makeFullPath($self->{deviceRootDir}, $storageName);

    return $self->_resolveSymlink($path);
  }
  return $self->getStoragePath();
}

########################################################################
# @inherit
##
sub setDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  $self->Permabit::BlockDevice::VDO::setDeviceName($deviceName);
  # Override the vdo name to point at the VDO pool.
  my $vgName             = $self->{volumeGroup}->getName();
  $self->{vdoDeviceName} = "$vgName-$self->{deviceName}pool-vpool";
}

########################################################################
# @inherit
##
sub setSymbolicPath {
  my ($self) = assertNumArgs(1, @_);
  $self->Permabit::BlockDevice::LVM::setSymbolicPath();
  # Override the vdo symbolic path to point at the VDO pool.
  my $vgName     = $self->{volumeGroup}->getName();
  my $deviceName = "$vgName-$self->{deviceName}pool-vpool";

  $self->{vdoSymbolicPath}
    = makeFullPath($self->{deviceRootDir}, $deviceName);
}

########################################################################
# @inherit
##
sub forceRebuild {
  my ($self) = assertNumArgs(1, @_);

  $self->runVDOForceRebuild();
  $self->{expectIndexer} = 1;
  $self->recover();
}

########################################################################
# Change a setting of the VDO volume via lvchange.
#
# @param setting  the arguments to pass to lvchange
##
sub _changeVDOSetting {
  my ($self, $setting) = assertNumArgs(2, @_);
  my $name = "$self->{deviceName}pool";
  $self->{volumeGroup}->_changeLogicalVolume($name, $setting);
}

########################################################################
# @inherit
##
sub disableCompression {
  my ($self) = assertNumArgs(1, @_);
  if ($self->isVDOCompressionEnabled()) {
    $self->_changeVDOSetting("--compression n");
  } else {
    $log->info("compression already disabled");
  }
}

########################################################################
# @inherit
##
sub disableDeduplication {
  my ($self) = assertNumArgs(1, @_);
  if ($self->isVDODedupeEnabled()) {
    $self->_changeVDOSetting("--deduplication n");
  } else {
    $log->info("deduplication already disabled");
  }
}

########################################################################
# @inherit
##
sub enableCompression {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->isVDOCompressionEnabled()) {
    $self->_changeVDOSetting("--compression y");
  } else {
    $log->info("compression already enabled");
  }
}

########################################################################
# @inherit
##
sub enableDeduplication {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->isVDODedupeEnabled()) {
    $self->_changeVDOSetting("--deduplication y");
  } else {
    $log->info("deduplication already enabled");
  }
}

1;
