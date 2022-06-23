##
# Perl object that represents a kernel dm-zero device plus a little
# bit of persistent storage at the beginning (where VDO keeps its
# superblock, the only thing that needs writing at initial format
# time).
#
# $Id$
##
package Permabit::BlockDevice::MostlyZero;

use strict;
use warnings FATAL => qw(all);
use Carp qw(confess);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEqualNumeric
  assertLENumeric
  assertNumArgs
);
use Permabit::SystemUtils qw(createRemoteFile);
use Permabit::Utils qw(makeFullPath parseBytes);

use base qw(Permabit::BlockDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple name of the device node
     deviceName            => "mz0",
     # @ple directory path containing the device node.
     deviceRootDir         => "/dev/mapper",
     # @ple amount of persistent storage to create, in bytes
     persistentStorageSize => 4096,
     # @ple total "storage" size
     size                  => '500G',
    );
##

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  my $size        = parseBytes($self->{size});
  my $sizeSectors = int($size / 512);
  assertEqualNumeric($sizeSectors * 512, $size,
                     "size must be a multiple of 512 bytes");

  my $persistentBytes   = parseBytes($self->{persistentStorageSize});
  my $persistentSectors = int($persistentBytes / 512);
  assertEqualNumeric($persistentSectors * 512, $persistentBytes,
                     "persistentStorageSize must be a multiple of 512 bytes");
  assertLENumeric($persistentBytes, $size,
                  "persistent size cannot be larger than device");

  my $zeroSectors = $sizeSectors - $persistentSectors;
  my $config;
  if ($self->{persistentStorageSize} > 0) {
    my $storageDevName = $self->{storageDevice}->getDevicePath();
    my $config1        = "0 $persistentSectors linear $storageDevName 8";
    my $config2        = "$persistentSectors $zeroSectors zero";
    $config            = "$config1\n$config2\n";
  } else {
    $config = "0 $zeroSectors zero\n";
  }
  my $configPath = makeFullPath($self->{scratchDir} || "/tmp",
                                "$self->{deviceName}.table");
  createRemoteFile($self->getMachine()->getName(), $config, $configPath);
  $self->runOnHost(["cat $configPath",
                    "sudo dmsetup create $self->{deviceName} $configPath"],
                   "\n");
  $self->addDeactivationStep(sub { $self->deactivateMostlyZeroDevice(); });

  $self->runOnHost(["sudo dmsetup status",
                    "sudo dmsetup table",
                    "sudo dmsetup info $self->{deviceName}"],
                   "\n");

  # Always call SUPER::activate at end to do final initialization
  $self->SUPER::activate();
}

########################################################################
# @inherit
##
sub deactivateMostlyZeroDevice {
  my ($self) = assertNumArgs(1, @_);
  $self->getMachine()->dmsetupRemove($self->{deviceName});
}

1;
