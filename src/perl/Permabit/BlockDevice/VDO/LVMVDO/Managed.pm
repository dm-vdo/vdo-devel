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
                                        $self->getLVMVDOName(),
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

  # resize the device that sits on top of the vdo pool
  my $name = $self->{deviceName};

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
  my $name = $self->getLVMVDOName();

  my $newSize = $self->getMetadataSize() + $physicalSize;
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
  $self->{volumeGroup}->renameLogicalVolume($self->{deviceName}, $newName);
  $self->{volumeGroup}->renameLogicalVolume($self->getLVMVDOName(), $newName . "pool");
  $self->setDeviceName($newName);
}

########################################################################
# @inherit
##
sub getLogicalMetadataSize {
  my ($self) = assertNumArgs(1, @_);
  return $MB;
}

########################################################################
# Get the lvm name of the vdo device.
##
sub getLVMVDOName {
  my ($self) = assertNumArgs(1, @_);
  return "$self->{deviceName}pool";
}

########################################################################
# Get the lvm name of the vdo storage device.
##
sub getLVMStorageName {
  my ($self) = assertNumArgs(1, @_);
  my $vgName = $self->{volumeGroup}->getName();
  return "$self->{deviceName}pool_vdata";
}

########################################################################
# @inherit
##
sub setDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  my $vgName             = $self->{volumeGroup}->getName();
  $self->{vdoDeviceName} = "$vgName-$self->{deviceName}pool-vpool";
  $self->Permabit::BlockDevice::LVM::setDeviceName($deviceName);
}

1;
