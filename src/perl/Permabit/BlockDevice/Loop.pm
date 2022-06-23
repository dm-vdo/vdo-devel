##
# Perl object that represents a Loop Device
#
# $Id$
##
package Permabit::BlockDevice::Loop;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Basename qw(basename);
use Log::Log4perl;
use Permabit::Assertions qw(assertDefined assertNumArgs);
use Permabit::Constants qw($KB);

use base qw(Permabit::BlockDevice::Bottom);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{inherited}
our %BLOCKDEVICE_INHERITED_PROPERTIES
  = (
     # @ple physical size of the underlying storage
     loopSize => undef,
    );
##

########################################################################
# @inherit
##
sub checkStorageDevice {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::checkStorageDevice();
  assertDefined($self->{loopSize});
}

########################################################################
# @inherit
##
sub setDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  my $devicePath = $self->runOnHost("sudo losetup -f");
  chomp($devicePath);
  $self->SUPER::setDeviceName(basename($devicePath));
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  # Create the backing file.
  my $deviceName        = $self->getDeviceName();
  $self->{_storagePath} = "/u1/$deviceName";
  my $BLOCK_SIZE        = 4 * $KB;
  $self->getMachine()->dd(if    => "/dev/zero",
                          of    => $self->{_storagePath},
                          bs    => $BLOCK_SIZE,
                          count => $self->{loopSize} / $BLOCK_SIZE,
                         );

  # Set up the loop device.
  my $devicePath = $self->getDevicePath();
  $self->runOnHost("sudo losetup $devicePath $self->{_storagePath}");
  $self->addDeactivationStep(sub { $self->deactivateLoopDevice(); });

  # Always call SUPER::activate() at end to do final initialization.
  $self->SUPER::activate();
}

########################################################################
# Deactivate this loop device.
##
sub deactivateLoopDevice {
  my ($self) = assertNumArgs(1, @_);
  $self->runOnHost(["sudo losetup -d " . $self->getDevicePath(),
                    "sudo rm $self->{_storagePath}"]);
}

1;
