##
# Perl object that represents a raw Linux block device
#
# $Id$
##
package Permabit::BlockDevice::Raw;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertNumArgs
);

use base qw(Permabit::BlockDevice::Bottom);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

# TODO: * Add a concept of "partitions" instead of just having them
#         be separate block devices as they are now.

########################################################################
# @inherit
##
sub checkStorageDevice {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::checkStorageDevice();
  $self->{deviceName} //= $self->getMachine()->selectDefaultRawDevices()->[0];
}

1;
