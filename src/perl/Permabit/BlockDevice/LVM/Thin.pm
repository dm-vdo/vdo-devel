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
sub setup {
  my ($self) = assertNumArgs(1, @_);
  $self->{volumeGroup}->createThinVolume($self->{deviceName},
                                         $self->{lvmSize});
  $self->SUPER::setup();
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::teardown();
  $self->{volumeGroup}->deleteThinVolume($self->{deviceName});
}

1;
