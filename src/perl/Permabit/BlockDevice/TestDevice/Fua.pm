##
# Perl object that represents a block device that adds the FUA bit to any
# write requests
#
# $Id$
##
package Permabit::BlockDevice::TestDevice::Fua;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);

use base qw(Permabit::BlockDevice::TestDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple fua frequency (0 implies stripping all flushes and fuas)
     fuaFrequency => 1,
     # @ple the kernel module name
     moduleName   => 'pbitfua',
     # @ple the target type
     target       => 'fua',
    );
##

########################################################################
# @inherit
##
sub makeTableLine {
  my ($self) = assertNumArgs(1, @_);
  return join(' ',
              $self->startTableLine(),
              $self->{storageDevice}->getDevicePath(),
              $self->{fuaFrequency});
}

1;
