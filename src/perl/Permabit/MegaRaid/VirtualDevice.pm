########################################################################
# Class representing a virtual device created by a MegaRaid::Adapter object.
#
# These objects can be created and destroyed by the Adapter class. This class
# takes a VirtualDeviceConfig object which describes what disks this device
# should assign itself to and what RAID level it is, among other things.
#
# $Id$
##
package Permabit::MegaRaid::VirtualDevice;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp qw(confess);
use Devel::StackTrace;

use Permabit::Assertions qw(
  assertDefined
  assertLENumeric
  assertMinMaxArgs
  assertNumArgs
  assertNumDefinedArgs
  assertTrue
  assertType
);
use Permabit::Utils qw(makeFullPath);
use Scalar::Util qw(refaddr);
use Storable qw(dclone);

use overload q("")    => \&_as_string,
             q(eq)    => \&_equals,
             q(ne)    => sub {!_equals(@_)},
             q(==)    => \&_num_equals,
             q(!=)    => sub {!_num_equals(@_)};

use base qw(Permabit::BlockDevice::Bottom);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple The configuration that describes this virtual device.  The config
     #      is used to initialize the device and can be discarded after the
     #      ctor returns.
     config        => undef,
     # @ple directory path containing the device node.
     deviceRootDir => "/dev/disk/by-path",
    );
##

our %BLOCKDEVICE_INHERITED_PROPERTIES
  = (
     # The PCI address for the adapter
     pciAddress => undef,
    );

########################################################################
# @inherit
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2, @_);
  assertDefined($arguments->{config});
  assertDefined($arguments->{deviceName});
  assertType('Permabit::MegaRaid::VirtualDeviceConfig', $arguments->{config});
  $self->SUPER::configure($arguments);
  assertDefined($self->{pciAddress});

  # Make a copy of the config so it can be reused by the caller and so it
  # doesn't change out from under us.
  assertLENumeric(1, scalar($self->{config}->getDisks()),
                  "the config needs to contain at least one Physical Disk");
  $self->{_config} = $self->{config}->clone();
  delete($self->{config});
}

########################################################################
# Activate the configuration. This is called as setup from
# StorageStack::make().
##
sub setup {
  my ($self) = assertNumArgs(1, @_);
  $self->_activate();
}

########################################################################
# Copy ctor. Performs a deep copy except the PhysicalDisks are not copied
##
sub clone {
  my ($self) = assertNumDefinedArgs(1, @_);
  # Can't call new because everything is already initialized but it
  # should be ok to get our own copy of the config.
  return bless { %$self,
                 _config => $self->getConfig()},
               ref($self);
}

########################################################################
# Activates a configuration for a given VirtualDevice. This associates the
# PhysicalDisks of this configuration with the given VirtualDevice since a
# disk can only be associated to a single virtual device at a time.
##
sub _activate {
  my ($self) = assertNumDefinedArgs(1, @_);
  assertTrue($self->{_config}->isAllDisksFree(),
             "all disks in config are not free");
  foreach my $disk ($self->getDisks()) {
    $disk->joinDevice($self);
  }
}

########################################################################
# @inherit
##
sub setSymbolicPath {
  my ($self) = assertNumArgs(1, @_);
  # Where does "2" come from?
  my $dev = "pci-$self->{pciAddress}-scsi-0:2:$self->{deviceName}:0";
  $self->{symbolicPath} = makeFullPath($self->{deviceRootDir}, $dev);
}

########################################################################
# When the object goes away, we should release the disks so other devices
# can use them
##
sub DESTROY {
  my ($self) = assertNumDefinedArgs(1, @_);
  $self->{stack}->destroy($self);
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumDefinedArgs(1, @_);
  foreach my $disk ($self->getDisks()) {
    $disk->leaveDevice();
  }

  # Clear the disk array because we no longer own them and do other
  # cleanup that perl's GC should do anyway but someone might be
  # calling this method implicitly.
  $self->{_config}->setDisks([]);
  $self->{machine} = undef;
  $self->SUPER::teardown();
}

########################################################################
# Returns a copy of this devices configuration. We don't give direct access
# to the config because this device is the only thing that should own it but
# copies of the config are useful in cases such as wiping and rebuilding a
# replica of a given virtual device.
#
# @return a VirtualDeviceConfig object
##
sub getConfig {
  my ($self) = assertNumDefinedArgs(1, @_);
  return $self->{_config}->clone();
}

########################################################################
# Returns the list of disks associated with this device
#
# @return a list of PhysicalDisk objects
##
sub getDisks {
  my ($self) = assertNumDefinedArgs(1, @_);
  return $self->{_config}->getDisks();
}

########################################################################
# Return 1 if this VirtualDevice is mounted, else 0
#
# @return bool; devicePath is defined
##
sub isMounted {
  my ($self) = assertNumDefinedArgs(1, @_);
  return !$self->{machine}->debugSendCmd('mount | grep "'
                                         . $self->getDevicePath() . '"');
}

########################################################################
# Overload eq to perform a deep equality check to compare two virtual devices
# for likeness.
##
sub _equals {
  my ($self, $vd) = @_;
  # NOTE: Maybe we should ignore the ID (deviceName) field so we can compare
  #       disks that look the same (same phys disks and config settings) but
  #       got assigned a different ID and devicePath?

  # Check if we're dealing with a VirtualDevice or not
  if (! ref($vd) || ! eval { $vd->isa(__PACKAGE__)}) {
    return 0;
  }

  # Check field count -- this should always pass
  if (scalar(keys %{$self}) != scalar(keys %{$vd})) {
    return 0;
  }

  # Check that the configurations match
  if ($self->{_config} ne $vd->{_config}) {
    return 0;
  }

  # Check that the same fields are defined and equal
  # XXX subtlety warning: the machine object will get stringified (which
  #     is overloaded) and the strings will be compared
  foreach my $key (keys %{$self}) {
    if ($key eq "_config") {
      # We handle this above
      next;
    }
    if (defined($self->{$key}) != defined($vd->{$key})) {
      return 0;
    } elsif (defined($self->{$key}) && ("$self->{$key}" ne "$vd->{$key}")) {
      return 0;
    }
  }
  return 1;
}

########################################################################
# Overload == to check that the objects are the same instance
##
sub _num_equals {
  my ($self, $vdc) = @_;
  return refaddr($self) == refaddr($vdc);
}

########################################################################
# Overload default stringification
##
sub _as_string {
  my $self = shift;
  return "VirtualDevice(id:$self->{deviceName} conf:$self->{_config})";
}

1;
