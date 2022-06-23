##
# Perl object that represents a kernel dm-crypt device
#
# $Id$
##
package Permabit::BlockDevice::Crypt;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertNumArgs
  assertType
);
use Permabit::Utils qw(makeFullPath);

use base qw(Permabit::BlockDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple name of the device node.
     deviceName    => "crypt0",
     # @ple directory path containing the device node.
     deviceRootDir => "/dev/mapper",
    );
##

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  my $keyFile = makeFullPath($self->getMachine()->{scratchDir},
                             'cryptsetup-key');
  $self->runOnHost("sudo dd if=/dev/urandom of=$keyFile bs=512 count=1");

  my $storagePath = $self->{storageDevice}->getDevicePath();
  $self->runOnHost("sudo cryptsetup luksFormat -v $storagePath $keyFile", 1);
  $self->addDeactivationStep(sub { $self->wipeLUKSHeader(); }, 0);

  $self->runOnHost("sudo cryptsetup luksOpen -v --key-file=$keyFile"
                    . " $storagePath $self->{deviceName} < /dev/null", 1);
  $self->addDeactivationStep(sub { $self->removeCryptoDevice(); });

  $self->runOnHost(["sudo dmsetup status",
                    "sudo dmsetup table",
                    "sudo dmsetup info $self->{deviceName}",
                    "sudo cryptsetup status $self->{deviceName}"],
                   1);

  $self->SUPER::activate();
}

######################################################################
# Forcibly wipe the LUKS partition header (otherwise SLES complains).
##
sub wipeLUKSHeader {
  my ($self) = assertNumArgs(1, @_);
  my $backing = $self->getStorageDevice();
  $log->info("Forcibly wiping LUKS header on $backing");
  $backing->ddWrite(bs => 512, count => 1, if => "/dev/zero");
}

######################################################################
# Remove the crypto device.
##
sub removeCryptoDevice {
  my ($self) = assertNumArgs(1, @_);
  $log->info("shutting down crypto device");
  $self->runOnHost("sudo cryptsetup remove $self->{deviceName}", 1);
}

1;
