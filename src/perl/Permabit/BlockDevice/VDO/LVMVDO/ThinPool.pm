##
# Perl object that represents an LVM Thin Pool using a VDO as data device
#
# $Id$
##
package Permabit::BlockDevice::VDO::LVMVDO::ThinPool;

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

use base qw(
  Permabit::BlockDevice::VDO::LVMVDO
  Permabit::BlockDevice::LVM::ThinPool
);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @inherit
##
sub setup {
  my ($self) = assertNumArgs(1, @_);

  # Get the location of the vdoformat binary for use by LVM.
  $self->{vdoformat} = $self->getMachine()->findNamedExecutable("vdoformat");

  # This will create a thin pool that has vdo set up to be the data device
  my $config = $self->makeLVMConfigString();
  my $totalSize = $self->getTotalSize($self->{physicalSize});
  $self->{volumeGroup}->createThinPool($self->{deviceName},
                                       $totalSize,
                                       $config,
                                       1);
  $self->Permabit::BlockDevice::VDO::setup();
}

########################################################################
# Rename a VDO device. Currently LVM managed devices are the only
# ones that support rename.
#
# @param newName The new name for the VDO device.
##
sub renameVDO {
  my ($self, $newName) = assertNumArgs(2, @_);
  my $name = $self->{deviceName};
  $self->{volumeGroup}->renameLogicalVolume($name, $newName);
  $self->setDeviceName($newName);
}

########################################################################
# Get the lvm name of the vdo device.
##
sub getLVMVDOName {
  my ($self) = assertNumArgs(1, @_);
  return "$self->{deviceName}_vpool0";
}

########################################################################
# Get the lvm name of the vdo storage device.
##
sub getLVMStorageName {
  my ($self) = assertNumArgs(1, @_);
  return "$self->{deviceName}_vpool0_vdata";
}

########################################################################
# @inherit
##
sub setDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  my $vgName             = $self->{volumeGroup}->getName();
  $self->{vdoDeviceName} = "$vgName-$self->{deviceName}_vpool0-vpool";
  $self->Permabit::BlockDevice::LVM::setDeviceName($deviceName);
}

1;
