########################################################################
# Class representing a given host's MegaRaid adapter.  It Provides methods
#  for creating MegaRaid virtual devices and gathering information about
#  existing configurations.
#
# Note: Methods exist in this class that will do destructive actions on
#  real block devices and filesystems.  Stick with the recommendated usecases
#  explained below whenever possible.
#
# @synopsis
#
# Create a MegaRaid::Adapter object given the necessary paramaters. All
# "REQUIRED" paramaters should be passed in.
#
#   my $adapter    = Permabit::MegaRaid::Adapter->new(%params);
#
# Get a MegaRaid virtual device and retrieve its device path
#
#   my $device     = $adapter->getVirtualDevices()->[0];
#   my $devicePath = $device->getDevicePath();
#
#  At this point $device is ready to use, and its drives should not be in
#   a steady state.
#
# $Id$
##
package Permabit::MegaRaid::Adapter;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use File::Basename;
use File::Path;
use Module::Load;
use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertLTNumeric
  assertNumDefinedArgs
  assertTrue
  assertType
);
use Permabit::MegaRaid::PhysicalDisk;
use Permabit::MegaRaid::Utils qw(
  parseCliResult
  chunkCliOut
);
use Permabit::MegaRaid::VirtualDevice;
use Permabit::MegaRaid::VirtualDeviceConfig;
use Permabit::StorageStack;
use Permabit::SystemUtils qw(assertCommand);
use Permabit::Utils qw(mergeToHash);
use Storable qw(dclone);

use base qw(Permabit::BinaryFinder);

# This class queries the adapter once about it's configuration, at new().
#  From that point on, this object keeps it's own state, knowing how megaraid
#  adapters "work".
# XXX Better to query the adapter (via megacli) as we make changes to it,
#      verifing our internal state?
#     Might be better if this was a Singlton so users didn't have to
#      explicitly hang on to it for everything to function.

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

# There's a single Adapter on our machines currently
my $ADAPTER_ID = 0;

########################################################################
# @paramList{new}
my %properties = (
  # @ple The number of failed disks
  failedDiskCt => undef,
  # @ple The firmware version
  fwVers       => undef,
  # @ple Permabit::UserMachine the adapter is on
  machine      => undef,
  # @ple the PCI address for the adapter
  pciAddress   => undef,
  # @ple The directory in which to run the commands
  runDir       => "/opt/MegaRAID/MegaCli",
  # @ple Don't use a BinaryFinder
  standAlone   => 1,
  # @ple Factory for creating cli objects. Defined here so we can
  #      override it for testing purposes.
  _cliFactory  => 'Permabit::MegaRaid::MegaCli',
  # @ple The PhysicalDisks objects
  _physDisks   => undef,
  # @ple Array ref of virtual device objects
  _virtDevices => [],
);
##

########################################################################
# Creates a C<Permabit::MegaRaid::Adapter> object
#
# @return a new C<Permabit::MegaRaid::Adapter> object
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self  = bless( { %{ dclone(\%properties) }, @_ }, $class);

  assertType("Permabit::UserMachine", $self->{machine});
  load $self->{_cliFactory};

  $self->_setAdapterInfo();

  return $self;
}

########################################################################
# Return our configured VirtualDevice objects
#
# @return an array ref of configured VirtualDevices
# @croaks if no configured virtual devices exist
##
sub getVirtualDevices {
  my ($self) = assertNumDefinedArgs(1, @_);
  my @configured = grep { defined($_) } @{$self->{_virtDevices}};
  assertLTNumeric(0, scalar(@configured), "No devices configured");
  return \@configured;
}

########################################################################
# Return our PhysicalDisk objects
#
# @return an array ref of PhysicalDisk objects
# @croaks if no physical drives were found on this adapter
##
sub getPhysDisks {
  my ($self) = assertNumDefinedArgs(1, @_);
  assertLTNumeric(0, scalar(@{$self->{_physDisks}}),
                  "no physical disks were found");
  # create a new array so that the caller can't modify ours
  return [@{$self->{_physDisks}}];
}

########################################################################
# Make a VirtualDevice object for a device on this adapter.
#
# @param vdc    the configuration of the device to create
##
sub makeVirtualDevice {
  my ($self, $vdc) = assertNumDefinedArgs(2, @_);
  assertType("Permabit::MegaRaid::VirtualDeviceConfig", $vdc);

  $self->{pciAddress} ||= $self->_getPCIAddress();
  # Create our VirtualDevice abstraction
  my $args = {machine    => $self->{machine},
              deviceName => $self->_getNextFreeIndx(),
              config     => $vdc, };
  if (!defined($self->{_storageStack})) {
    $self->{_storageStack} = Permabit::StorageStack->new($self);
  }

  return $self->{_storageStack}->create('virtual', $args);
}

########################################################################
# Add and initialize a given VirtualDevice.
#
# @param vdc    a VirtualDeviceConfig to create a VirtualDevice from
#
# @return the VirtualDevice
##
sub createVirtualDevice {
  my ($self, $vdc) = assertNumDefinedArgs(2, @_);
  my $device = $self->makeVirtualDevice($vdc);

  # Go and actually create the device on the card.
  $self->_getResult({ addVirtDev => 1, %{$vdc} });

  # And initialize it
  my $name = $device->getDeviceName();
  $self->_getResult({ initVirtDev => 1,
                      virtDevId   => $name,
                    });

  # The device should exist so we can start our object now and add it to our
  # known list of devices.
  $device->start();
  $self->{_virtDevices}->[$name] = $device;
  return $device;
}

########################################################################
# Unconfigure and remove an existing VirtualDevice from this Adapter.
#  unconfigure() is called on the VirtualDevice, and this Adapter object
#  releases its handle on it.  However, the client may hold on to the
#  the VirtualDevice object, to serve it's potential new purpose
#  of a VirtualDevice configuration.
#
# @param  vd        the VirtualDevice to remove
#
# @croaks if the given virtual device is not one we own
##
sub destroyVirtualDevice {
  my ($self, $vd) = assertNumDefinedArgs(2, @_);
  my $vdIndx = $self->_getVDIndx($vd);

  $self->_getResult({ deleteVirtDev => 1,
                      virtDevId     => $vdIndx });

  # Call implicitly because other things might still be hanging on to
  # references of this object and we really want to tear it down *NOW*
  $vd->DESTROY();

  # This leaves an undef array element by design.  The index of this array
  #  correlates directly to MegaRaid VirtualDevice IDs and is useful.
  $self->{_virtDevices}->[$vdIndx] = undef;
}

########################################################################
# Set information about our adapter configuration
##
sub _setAdapterInfo {
  my ($self) = assertNumDefinedArgs(1, @_);

  # Map this class' adapter fields to -AdpAllInfo megacli output "keys"
  my %ADAPTER_INFO_MAP = (
   "Disks"        => "_physDiskCt",
   "Failed Disks" => "failedDiskCt",
   "FW Version"   => "fwVers",
  );

  # Set adapter info
  my $cliOut      = $self->_getResult({ getAdapterInfo => 1 });
  my $adapterInfo = parseCliResult($cliOut, \%ADAPTER_INFO_MAP)->[$ADAPTER_ID];
  mergeToHash($self, %{$adapterInfo});

  # Set our PhysicalDisks
  $self->_setPhysicalDisks();

  # Set our VirtualDevices (membership state of some or all PhysicalDisks
  #   will be modified here)
  $self->_setVirtDevices();
}

########################################################################
# Set our PhysicalDisk objects based on actual physical disks that exist
#  on our MegaRaid adapter.
##
sub _setPhysicalDisks {
  my ($self) = assertNumDefinedArgs(1, @_);

  # Map some virtual device fields to -PDList megacli output "keys"
  my %DISK_INFO_MAP = (
   "Device Id"           => "deviceId",
   "Enclosure Device ID" => "enclosureId",
   "Slot Number"         => "slotNum",
  );

  my $cliOut = $self->_getResult({ getPhysicalDiskInfo => 1 });
  my $all    = parseCliResult($cliOut, \%DISK_INFO_MAP);

  my @diskObjects;
  foreach my $diskInfo (@{$all}) {
    push(@{$self->{_physDisks}},
         Permabit::MegaRaid::PhysicalDisk->new(%{$diskInfo},
                                               machine  => $self->{machine}));
  }
}

########################################################################
# Set information about existing configured virtual devices on the actual
#  MegaRaid adapter.
##
sub _setVirtDevices {
  my ($self) = assertNumDefinedArgs(1, @_);

  # Get groups of PhysicalDisk objects, grouped by VirtualDevice membership
  my $physDiskGroups = $self->_getVirtDevicePhysDiskGroups();

  # Create our VirtualDevice objects
  my %VIRT_DEVICE_INFO_MAP = (
   "Current Cache Policy" => "cachePolicyStr", # <- The value of this key contains
   "Disk Cache Policy"    => "diskCachePol",   #      readCachePol, writeCachePol,
   "RAID Level"           => "raidType",
   "Strip Size"           => "stripeSize",
  );

  my $cliOut         = $self->_getResult({ getPhysDiskVirtDevMap => 1 });
  my $virtDrivesInfo = parseCliResult($cliOut, { %VIRT_DEVICE_INFO_MAP });

  # Create our VirtualDevice objects, correctly assigning PhysicalDisk groups
  # to each.
  # XXX We should be parsing the vdIndx out of the cli output
  for (my $vdIndx = 0; $vdIndx < @{$virtDrivesInfo}; $vdIndx++) {
    my $vdArgs = $virtDrivesInfo->[$vdIndx];

    # The "Current Cache Policy" needs to be split into 3 fields
    my $cachePol = _parseCachePolicyStr($vdArgs->{cachePolicyStr});

    # Delete the cachePolicyStr key now that we're done munging, and
    #  VirtualDeviceConfig->new() doesn't need it
    delete $vdArgs->{cachePolicyStr};

    # Set the VirtualDeviceConfig->new() arguments we parsed
    $vdArgs->{writeCachePol}  = $cachePol->{writePol};
    $vdArgs->{readCachePol}   = $cachePol->{readPol};
    $vdArgs->{deviceCachePol} = $cachePol->{cachePol};

    # Clean up the raidType and stripeSize strings
    $vdArgs->{raidType}   =~ s/.*RAID Level Qualifier-(\d+)/$1/;
    $vdArgs->{stripeSize} =~ s/(\d+) KB/$1/;

    # Set the PhysicalDisk group for this VirtualDeviceConfig
    assertDefined($physDiskGroups->[$vdIndx],
                  "No PhysicalDisk group found for virtual device $vdIndx");
    $vdArgs->{disks} = $physDiskGroups->[$vdIndx];

    my $vdc    = Permabit::MegaRaid::VirtualDeviceConfig->new(%{$vdArgs});
    my $device = $self->makeVirtualDevice($vdc);
    $device->start();
    $self->{_virtDevices}->[$vdIndx] = $device;
  }
}

########################################################################
# Look up the PCI address for the MegaRaid adapter.
##
sub _getPCIAddress {
  my ($self) = assertNumDefinedArgs(1, @_);
  my $machine = $self->{machine};
  my $pattern = "/sys/bus/pci/drivers/megaraid_sas/[0-9]*:*:*.*";
  my $result = $machine->sendCommand("/bin/ls -1d $pattern");
  assertEqualNumeric(0, $result, "found megaraid_sas device");
  my $output = $machine->getStdout();
  chomp($output);
  my @paths = split("\n", $output);
  assertEqualNumeric(1, scalar(@paths),
                     "found exactly one megaraid_sas device");
  return basename($paths[0]);
}

########################################################################
# Get groups of PhysicalDrive objects, grouped by associated VirtualDevice
#
# XXX This method could use some simplification.  It bends over backwards a
#  bit using chunkCliOut() (which was created for this method alone), just
#  to go on and use parseCliOut(), which actually does MORE than what we need,
#  forcing us to do more work.
#
# @return an array ref of PhysicalDisk groups
##
sub _getVirtDevicePhysDiskGroups {
  my ($self) = assertNumDefinedArgs(1, @_);

  # The array of PhysicalDisk object groups to return.
  my @diskGroups;

  my $cliOut = $self->_getResult({ getPhysDiskVirtDevMap => 1 });

  # Create groups of cli output lines.  One group per virtual device.
  my $textGroups = chunkCliOut($cliOut, "Virtual Drive:");

  # Feed each MegaCli text group to parseCliResult() and get arrays of
  #  physDisk { id => value } groups
  # XXX this is ugly
  my @idHashes
    = map { parseCliResult($_, { "Device Id" => "id" }) } @{$textGroups};

  # Fetch our PhysicalDisks by their IDs
  # XXX ugly (But fully operational!)
  foreach my $idGroup (@idHashes) {
    my @ids           = map { values %{$_} } @{$idGroup};
    my @physDriveObjs = map { $self->_getPhysDiskByDeviceId($_) } @ids;
    push(@diskGroups, \@physDriveObjs);
  }

  return \@diskGroups;
}

########################################################################
# Get an existing PhysicalDisk object, given a PhysicalDisk deviceId.
#
# @param devId  the PhysicalDisk deviceId
#
# @return the PhysicalDisk
# @croaks if a corresponding PhysicalDisk does not exist
##
sub _getPhysDiskByDeviceId {
  my ($self, $devId) = assertNumDefinedArgs(2, @_);
  foreach my $disk (@{$self->{_physDisks}}) {
    if ($disk->{deviceId} == $devId) {
      return $disk;
    }
  }
  $log->logcroak("No PhysicalDisk with deviceId: $devId exists");
}

########################################################################
# Get the VirtualDevice index.  Where in the _virtDevices array this
#  VirtualDevice exists.
#
# @param vd  the VirtualDevice
#
# @return the index
##
sub _getVDIndx {
  my ($self, $vd) = assertNumDefinedArgs(2, @_);
  assertType("Permabit::MegaRaid::VirtualDevice", $vd);

  for (my $vdIndx = 0; $vdIndx < @{$self->{_virtDevices}}; $vdIndx++) {
    if ($vd == $self->{_virtDevices}->[$vdIndx]) {
      return $vdIndx;
    }
  }

  $log->logcroak("Could not find index of given VirtualDevice");
}

########################################################################
# Gets the next available VirtualDevice index for this adapter.
#  VirtualDevice exists.
#
# @return the next available index
##
sub _getNextFreeIndx {
  my ($self) = assertNumDefinedArgs(1, @_);
  for (my $vdIndx = 0; $vdIndx <= scalar(@{$self->{_virtDevices}}); $vdIndx++) {
    if (! defined($self->{_virtDevices}->[$vdIndx])) {
      return $vdIndx;
    }
  }
}

########################################################################
# Get the result from a MegaCli call given a set of params
#
# @param params  hash ref of parameters to send to MegaCli->new()
#
# @return the MegaCli stdout result
##
sub _getResult {
  my ($self, $params) = assertNumDefinedArgs(2, @_);

  # We only have one adapter on our hosts
  my %defaults = (adapterId => $ADAPTER_ID,
                  host      => $self->{machine}->getName());

  my $cli = $self->{_cliFactory}->new($self, {%defaults, %{$params}});
  return $cli->run()->{stdout};
}

########################################################################
# Parse a given virtual device's Current Cache Policy MegaCli output string
#
# @param cpStr  the cache policy string to parse
#
# @return a hash ref { writePol, readPol, cachePol }
##
sub _parseCachePolicyStr {
  my ($cpStr) = assertNumDefinedArgs(1, @_);
  my ($writePol, $readPol, $cachePol) = split(/, /, $cpStr);
  assertTrue(defined($writePol) && defined($readPol) && defined($cachePol));
  # XXX Need to check that we're going to match all valid patterns
  assertTrue($writePol =~ /Write(Through|Back)/           || 0);
  assertTrue($readPol  =~ /Read/, "unrecognized $readPol" || 0);
  assertTrue($cachePol =~ /(Direct|Cached)/               || 0);
  return { writePol => $writePol,
           readPol  => $readPol,
           cachePol => $cachePol };
}

1;
