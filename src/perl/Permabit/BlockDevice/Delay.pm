##
# Perl object that represents a kernel dm-delay device.
#
# $Id$
##
package Permabit::BlockDevice::Delay;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(Permabit::BlockDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple name of the device node.
     deviceName    => "delay0",
     # @ple directory path containing the device node.
     deviceRootDir => "/dev/mapper",
     # @ple read delay in milliseconds
     readDelay     => 50,
    );
##

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  my $table = join(" ",
                   0,
                   int($self->{storageDevice}->getSize() / $SECTOR_SIZE),
                   "delay",
                   $self->{storageDevice}->getSymbolicPath(),
                   0,
                   $self->{readDelay},
                  );
  $self->runOnHost("sudo dmsetup create $self->{deviceName} --table '$table'");
  my $cleaner = sub {
    $self->getMachine()->dmsetupRemove($self->getDeviceName());
  };
  $self->addDeactivationStep($cleaner);
  $self->SUPER::activate();
}

1;
