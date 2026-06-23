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
use Permabit::Assertions qw(assertNumArgs assertTrue);

use base qw(Permabit::BlockDevice::LVM);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @inherit
##
sub checkStorageDevice {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::checkStorageDevice();
  # Make sure the storage device is a ThinPool
  assertTrue($self->{storageDevice}->isa("Permabit::BlockDevice::LVM::ThinPool"));
  # Use the thinpool's volume group
  $self->{volumeGroup} = $self->{storageDevice}->{volumeGroup};
  # Use the thinpool's size instead of volume group size which LVM.pm uses
  $self->{lvmSize} //= $self->{storageDevice}->getSize();
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
