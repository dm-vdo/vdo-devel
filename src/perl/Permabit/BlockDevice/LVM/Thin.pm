##
# Perl object that represents a Thin Volume managed by LVM
#
# $Id$
##
package Permabit::BlockDevice::LVM::Thin;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(Permabit::BlockDevice::LVM);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @inherit
##
sub checkStorageDevice {
  my ($self) = assertNumArgs(1, @_);
  # This will create the bottom device if needed.
  $self->SUPER::checkStorageDevice();
  # If our storage device is a thin pool (this includes vdo thin pools),
  # don't do anything, just use it.
  if ($self->{storageDevice}->isa("Permabit::BlockDevice::LVM::ThinPool")) {
    $self->{volumeGroup} = $self->{storageDevice}->{volumeGroup};
    return;
  }
  # If we don't have a thin pool already, create a generic one.
  $self->{storageDevice} = $self->{stack}->create("thinpool")
}

########################################################################
# @inherit
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2, @_);
  $self->SUPER::configure($arguments);
  $self->{lvmSize} = $self->{storageDevice}->getSize();
  $self->{lvmSize} = $self->{volumeGroup}->alignToExtent($self->{lvmSize});
}

########################################################################
# @inherit
##
sub setup {
  my ($self) = assertNumArgs(1, @_);
  $self->{volumeGroup}->createThinVolume($self->{storageDevice},
                                         $self->{deviceName},
                                         $self->{lvmSize});
  $self->SUPER::setup();
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::teardown();
  $self->{volumeGroup}->deleteLogicalVolume($self->{deviceName});
}

1;
