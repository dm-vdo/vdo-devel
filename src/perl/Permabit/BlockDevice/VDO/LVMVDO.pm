##
# Base Perl object that represents a VDO Volume managed by LVM
#
# $Id$
##
package Permabit::BlockDevice::VDO::LVMVDO;

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
  confess("Failed to override the growLogical method");
}

########################################################################
# @inherit
##
sub growPhysical {
  my ($self, $physicalSize) = assertNumArgs(2, @_);
  confess("Failed to override the growPhysical method");
}

########################################################################
# Get the lvm name of the device on top of VDO. Implemented in inheriting
# classes.
##
sub getLVMName {
  my ($self) = assertNumArgs(1, @_);
  confess("Failed to override the getLVMName method");
}

########################################################################
# Get the lvm name of the vdo device. Implemented in inheriting classes.
##
sub getLVMVDOName {
  my ($self) = assertNumArgs(1, @_);
  confess("Failed to override the getLVMVDOName method");
}

########################################################################
# Get the lvm name of the vdo storage device. Implemented in inheriting
# classes.
##
sub getLVMStorageName {
  my ($self) = assertNumArgs(1, @_);
  confess("Failed to override the getLVMStorageName method");
}

########################################################################
# Get the name of the vdo storage device. Implemented in inheriting
# classes.
##
sub getVDOStorageName {
  my ($self) = assertNumArgs(1, @_);
  confess("Failed to override the getVDOStorageName method");
}

########################################################################
# @inherit
##
sub getVDOStoragePath {
  my ($self) = assertNumArgs(1, @_);
  if (defined($self->{volumeGroup})) {
    my $vgName      = $self->{volumeGroup}->getName();
    my $storageName = "$vgName-" . $self->getLVMStorageName();
    my $path        = makeFullPath($self->{deviceRootDir}, $storageName);

    return $self->_resolveSymlink($path);
  }
  return $self->getStoragePath();
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
  confess("Failed to override the renameVDO method");
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
  my $storageName = $self->getLVMStorageName();
  my $vgName   = $self->{volumeGroup}->getName();
  my $fullName = "$vgName-$storageName";

  $self->runOnHost("sudo dmsetup remove $fullName");
  $self->SUPER::disableWritableStorage();
}

########################################################################
# @inherit
##
sub enableWritableStorage {
  my ($self) = assertNumArgs(1, @_);
  my $storageName = $self->getLVMStorageName();

  $self->{volumeGroup}->enableLogicalVolume($storageName);

  my $vgName   = $self->{volumeGroup}->getName();
  my $fullName = "$vgName-$storageName";
  my $table = $self->runOnHost("sudo dmsetup table $fullName");

  $self->{volumeGroup}->disableLogicalVolume($storageName);

  $self->runOnHost("sudo dmsetup create $fullName --table '$table'");
  $self->SUPER::enableWritableStorage();
}

########################################################################
# @inherit
##
sub disableReadableStorage {
  my ($self) = assertNumArgs(1, @_);
  my $storageName = $self->getLVMStorageName();
  $self->{volumeGroup}->disableLogicalVolume($storageName);
  $self->SUPER::disableReadableStorage();
}

########################################################################
# @inherit
##
sub enableReadableStorage {
  my ($self) = assertNumArgs(1, @_);
  my $storageName = $self->getLVMStorageName();
  $self->{volumeGroup}->enableLogicalVolume($storageName);
  $self->SUPER::enableReadableStorage();
}

########################################################################
# @inherit
##
sub setSymbolicPath {
  my ($self) = assertNumArgs(1, @_);
  $self->{vdoSymbolicPath}
    = makeFullPath($self->{deviceRootDir}, $self->{vdoDeviceName});
  $self->Permabit::BlockDevice::LVM::setSymbolicPath();
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
  $self->{volumeGroup}->_changeLogicalVolume($self->getLVMVDOName(), $setting);
}

########################################################################
# @inherit
##
sub changeVDOSettings {
  my ($self, $args) = assertNumArgs(2, @_);
  my $settings =
    "--vdosettings '" . join(' ', map("$_=$args->{$_}", keys(%$args))) . "'";
  $self->_changeVDOSetting($settings);
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
