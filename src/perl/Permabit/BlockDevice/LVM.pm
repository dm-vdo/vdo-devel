##
# Perl object that represents a block device managed by LVM.
#
# $Id$
##
package Permabit::BlockDevice::LVM;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertNumArgs
);
use Permabit::Constants qw($KB $MB);
use Permabit::Utils qw(makeFullPath);
use Storable qw(dclone);

use base qw(Permabit::BlockDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $DEFAULT_LOGICAL_VOLUME_NAME = 'lvm';

# A counter to ensure that logical volume names are unique
my $defaultNameCounter = 0;

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple directory path containing the device node.
     deviceRootDir => "/dev/mapper",
     # @ple the volume group
     volumeGroup   => undef,
    );
##

########################################################################
# @paramList{inherited}
our %BLOCKDEVICE_INHERITED_PROPERTIES
  = (
     # @ple size of the logical volume
     lvmSize => undef,
    );
##

########################################################################
# @inherit
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2, @_);
  $self->SUPER::configure($arguments);
  if (!defined($self->{deviceName})) {
    $self->{deviceName}
      = ($DEFAULT_LOGICAL_VOLUME_NAME . $defaultNameCounter++);
  }

  $self->{volumeGroup}
    //= $self->{stack}->createVolumeGroup($self->{storageDevice});
  $self->{lvmSize} //= $self->{volumeGroup}->getFreeBytes();
  $self->{lvmSize}   = $self->{volumeGroup}->alignToExtent($self->{lvmSize});
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  $self->{volumeGroup}->enableLogicalVolume($self->{deviceName});
  $self->addDeactivationStep(sub { $self->disableLogicalVolume(); });

  # Always call SUPER::activate at end to do final initialization.
  $self->SUPER::activate();
}

########################################################################
# @inherit
##
sub disableLogicalVolume {
  my ($self) = assertNumArgs(1, @_);
  $self->{volumeGroup}->disableLogicalVolume($self->{deviceName});
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);
  $self->runOnHost("sync");
  $self->SUPER::teardown();
}

########################################################################
# Change the size of the logical device
#
# @param newSize  the new logical volume size as an absolute number
##
sub resize {
  my ($self, $newSize) = assertNumArgs(2, @_);
  $newSize = $self->{volumeGroup}->alignToExtent($newSize);
  if ($newSize != $self->getSize()) {
    $self->{volumeGroup}->resizeLogicalVolume($self->{deviceName}, $newSize);
    $self->{lvmSize} = $newSize;
  }
}

########################################################################
# Extend the size of the logical device
#
# @param newSize  the new logical volume size as an absolute number
##
sub extend {
  my ($self, $newSize) = assertNumArgs(2, @_);
  $newSize = $self->{volumeGroup}->alignToExtent($newSize);
  $self->{volumeGroup}->extendLogicalVolume($self->{deviceName}, $newSize);
  $self->{lvmSize} = $newSize;
}

########################################################################
# Reduce the size of the logical device
#
# @param newSize  the new logical volume size as an absolute number
##
sub reduce {
  my ($self, $newSize) = assertNumArgs(2, @_);
  $newSize = $self->{volumeGroup}->alignToExtent($newSize);
  $self->{volumeGroup}->reduceLogicalVolume($self->{deviceName}, $newSize);
  $self->{lvmSize} = $newSize;
}

########################################################################
# @inherit
##
sub setSymbolicPath {
  my ($self) = assertNumArgs(1, @_);
  my $vgName            = $self->{volumeGroup}->getName();
  my $name              = "$vgName-$self->{deviceName}";
  $self->{symbolicPath} = makeFullPath($self->{deviceRootDir}, $name);
}

1;
