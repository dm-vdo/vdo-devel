##
# Perl object that represents a RAID device that will sit
# at the bottom of the stack. This is a black box type
# device with no direct access to the devices the RAID
# is aggregating.
#
# $Id$
##
package Permabit::BlockDevice::RAID;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertGTNumeric
  assertNumArgs
);

use base qw(Permabit::BlockDevice::Bottom);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $DEFAULT_RAID_VOLUME_NAME = 'md';

# A counter to ensure that raid device names are unique
my $defaultNameCounter = 0;

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple array ref of device paths to create RAID from.
     raidPaths => undef,
     # @ple raid type to create
     raidType  => "0",
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
      = ($DEFAULT_RAID_VOLUME_NAME . $defaultNameCounter++);
  }
}

########################################################################
# Scans machine to determine what disks to create the raid from.
#
# @return arrayref of disk paths
##
sub getDiskPaths {
  my ($self) = assertNumArgs(1, @_);
  my $names = $self->getMachine()->selectDefaultRawDevices();
  return [ map { "/dev/$_" } @{$names} ];
}

########################################################################
# @inherit
##
sub checkStorageDevice {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::checkStorageDevice();
  $self->{raidPaths} //= $self->getDiskPaths();
  assertGTNumeric(scalar(@{$self->{raidPaths}}), 1);
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  my $name    = $self->{deviceName};
  my $type    = $self->{raidType};
  my @paths   = @{$self->{raidPaths}};
  my $count   = scalar(@paths);
  my $devices = join(" ", @paths);

  $self->runOnHost("yes | sudo mdadm --create /dev/$name $devices "
		   . " --level=$type --raid-devices=$count --force");

  $self->addDeactivationStep(sub { $self->stopRaid(); });

  $self->SUPER::activate();
}

########################################################################
# @inherit
##
sub stopRaid {
  my ($self) = assertNumArgs(1, @_);
  my $name = $self->{deviceName};
  $self->runOnHost("sudo mdadm --stop /dev/$name");
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::teardown();
  my @paths   = @{$self->{raidPaths}};
  my $devices = join(" ", @paths);
  $self->runOnHost("sudo mdadm --zero-superblock $devices");
}

1;
