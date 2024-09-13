##
# Perl object that represents a VDO Volume managed by LVM
#
# $Id$
##
package Permabit::BlockDevice::VDO::LVMVDO::Managed;

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

use base qw(Permabit::BlockDevice::VDO::LVMVDO);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @inherit
##
sub setup {
  my ($self) = assertNumArgs(1, @_);

  # Get the location of the vdoformat binary for use by LVM.
  $self->{vdoformat} = $self->getMachine()->findNamedExecutable("vdoformat"),

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
sub disableWritableStorage {
  my ($self) = assertNumArgs(1, @_);
  my $deviceName = "$self->{deviceName}pool_vdata";
  my $vgName   = $self->{volumeGroup}->getName();
  my $fullName = "$vgName-$deviceName";

  $self->runOnHost("sudo dmsetup remove $fullName");
  $self->SUPER::disableWritableStorage();
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
  $self->SUPER::enableWritableStorage();
}

########################################################################
# @inherit
##
sub disableReadableStorage {
  my ($self) = assertNumArgs(1, @_);
  $self->{volumeGroup}->disableLogicalVolume("$self->{deviceName}pool_vdata");
  $self->SUPER::disableReadableStorage();
}

########################################################################
# @inherit
##
sub enableReadableStorage {
  my ($self) = assertNumArgs(1, @_);
  $self->{volumeGroup}->enableLogicalVolume("$self->{deviceName}pool_vdata");
  $self->SUPER::enableReadableStorage();
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

1;
