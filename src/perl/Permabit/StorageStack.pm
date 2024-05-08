##
# Class to represent and manage the storage stack. Every VDOTest will have a
# StorageStack, and all BlockDevices must now be created and managed via a
# StorageStack.
#
# $Id$
##
package Permabit::StorageStack;

use strict;
use warnings FATAL => qw(all);
use Carp qw(
  confess
  croak
);
use English qw(-no_match_vars);

use Permabit::Assertions qw(
  assertEqualNumeric
  assertMinArgs
  assertMinMaxArgs
  assertNotDefined
  assertNumArgs
  assertType
);
use Permabit::Utils qw(hashExtractor);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %DEVICE_TYPES = (
  corruptor   => 'Permabit::BlockDevice::TestDevice::Managed::Corruptor',
  crypt       => 'Permabit::BlockDevice::Crypt',
  delay       => 'Permabit::BlockDevice::Delay',
  dory        => 'Permabit::BlockDevice::TestDevice::Dory',
  fua         => 'Permabit::BlockDevice::TestDevice::Fua',
  iscsi       => 'Permabit::BlockDevice::ISCSI',
  linear      => 'Permabit::BlockDevice::LVM::Linear',
  loop        => 'Permabit::BlockDevice::Loop',
  lvmvdo      => 'Permabit::BlockDevice::VDO::LVMManaged',
  mostlyzero  => 'Permabit::BlockDevice::MostlyZero',
  raid        => 'Permabit::BlockDevice::RAID',
  raw         => 'Permabit::BlockDevice::Raw',
  stripfua    => 'Permabit::BlockDevice::TestDevice::StripFua',
  thin        => 'Permabit::BlockDevice::LVM::Thin',
  tracer      => 'Permabit::BlockDevice::TestDevice::Managed::Tracer',
  upgrade     => 'Permabit::BlockDevice::VDO::Upgrade',
  vdo         => 'Permabit::BlockDevice::VDO::Unmanaged',
  virtual     => 'Permabit::MegaRaid::VirtualDevice',
);

my $DEFAULT_LOGICAL_VOLUME_GROUP = 'vdo';

########################################################################
# Get a list of supported device types.
##
sub getTypes {
  return sort(keys(%DEVICE_TYPES));
}

########################################################################
# Create a new storage stack.
#
# @param parent  The parent object for this stack
#
# @return The storage stack
##
sub new {
  my ($class, $parent) = assertNumArgs(2, @_);
  return bless {
    createHook      => undef,
    destroyHook     => undef,
    destroying      => -1,
    devices         => {},
    nextID          => 0,
    nextVolumeGroup => 0,
    parent          => $parent,
    root            => undef,
    teardownWrapper => \&_nullWrapper,
  }, $class;
}

######################################################################
# A default teardown wrapper which just invokes the supplied function.
#
# @param teardownFunction  The teardown function to invoke
##
sub _nullWrapper {
  my ($teardownFunction) = assertNumArgs(1, @_);
  $teardownFunction->();
}

########################################################################
# Set the hook to be called on device creation.
#
# @param hook  The hook to call when a device is created
##
sub setDeviceCreateHook {
  my ($self, $hook) = assertNumArgs(2, @_);
  $self->{createHook} = $hook;
}

########################################################################
# Set the hook to be called on device destruction. The hook is responsible for
# calling teardown on the device being destroyed.
#
# @param hook  The hook to call when a device is destroyed
##
sub setDeviceDestroyHook {
  my ($self, $hook) = assertNumArgs(2, @_);
  $self->{destroyHook} = $hook;
}

######################################################################
# Set a function to wrap around calls to teardown. The function should take
# one argument, a coderef, which it must invoke with no arguments.
#
# @param wrapper  The function to wrap around teardown
##
sub setTeardownWrapper {
  my ($self, $wrapper) = assertNumArgs(2, @_);
  $self->{teardownWrapper} = $wrapper // \&_nullWrapper;
}

########################################################################
# Create a block device and add it to the stack. If no underlying device is
# specified and the existing stack is neither empty nor branched, the new
# device will be backed by the current top of the stack. Some devices will
# default to adding layers below them in the stack if no stack already exists.
#
# @param  type        The type of device to create; supported types can be
#                     found by calling StorageStack->getTypes()
# @oparam properties  A hashref of additional properties
#
# @return The newly created device
##
sub create {
  my ($self, $type, $properties) = assertMinMaxArgs([{}], 2, 3, @_);
  $log->debug("Making device of type $type");
  my $class = $DEVICE_TYPES{$type};
  if (!defined($class)) {
    confess("$type is not a supported device type");
  }

  eval("use $class");
  if ($EVAL_ERROR) {
    confess($EVAL_ERROR);
  }

  my %arguments = %{$properties};
  assertNotDefined($arguments{stack});

  assertNotDefined($arguments{id});
  $arguments{id} = $self->{nextID}++;

  $arguments{typeName} //= $type;

  my $device = $class->new($self, { %arguments });
  $self->{devices}{$device->{id}} = $device;
  if ($self->isEmpty()) {
    $self->{root} = $device;
  }

  if ($device->{setupOnCreation}) {
    $device->setup();
  }

  if ($self->{createHook}) {
    $self->{createHook}->($device);
  }

  return $device;
}

########################################################################
# Stop all devices in order, top to bottom
##
sub stopAll {
  my ($self) = assertNumArgs(1, @_);
  $log->debug("Stopping all devices:");
  $self->apply(sub { $_[0]->stop() });
}

########################################################################
# Start all devices in order, bottom to top.
##
sub startAll {
  my ($self) = assertNumArgs(1, @_);
  $log->debug("Starting all devices:");
  $self->apply(sub { $_[0]->start() }, topDown => 1);
}

########################################################################
# Recover all devices in order, bottom to top.
##
sub recoverAll {
  my ($self) = assertNumArgs(1, @_);
  $log->debug("Recovering all devices:");
  $self->apply(sub { $_[0]->recover() }, topDown => 1);
}

########################################################################
# Destroy the entire stack.
##
sub destroyAll {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->isEmpty()) {
    $self->destroy($self->{root});
  }
}

########################################################################
# Destroy a device. If there are devices stacked on top of the device,
# they will be destroyed first.
#
# @param device  The device being torn down
##
sub destroy {
  my ($self, $device) = assertNumArgs(2, @_);
  $self->apply(sub { $self->destroyDevice($_[0]) },
               childSort => sub { return sort({ $a->{id} <=> $b->{id} } @_); },
               root      => $device);
}

######################################################################
# Destroy a device now that all of its children have been destroyed.
#
# @param device  The device to destroy
##
sub destroyDevice {
  my ($self, $device) = assertNumArgs(2, @_);
  $log->info("Destroying $device");

  my $id = $device->{id};

  delete $self->{devices}{$id};
  if ($self->{root} == $device) {
    $self->{root} = undef;
  }

  if ($self->{destroyHook}) {
    $self->{destroyHook}->($device);
  }

  $self->{destroying} = $id;
  $self->{teardownWrapper}(sub { $device->destroy() });
  $self->{destroying} = -1;
}

######################################################################
# Assert that a stack is destroying a device with a given ID.
##
sub assertDestroying {
  my ($self, $id) = assertNumArgs(2, @_);
  assertEqualNumeric($id, $self->{destroying});
}

########################################################################
# Get device properties from the parent.
#
# @param keys  An arrayref listing the keys to extract from the parent
#
# @return A hash of the requested properties
##
sub getParentProperties {
  my ($self, $keys) = assertNumArgs(2, @_);
  return hashExtractor($self->{parent}, $keys);
}

########################################################################
# Get the default UserMachine for this stack.
#
# @return The parent's UserMachine
##
sub getUserMachine {
  my ($self) = assertNumArgs(1, @_);
  return $self->{parent}->getUserMachine();
}

########################################################################
# Share the binary finder of a stack's parent.
#
# @param finder  The finder which wants to share with our parent
##
sub shareBinaryFinder {
  my ($self, $finder) = assertNumArgs(2, @_);
  $finder->shareBinaryFinder($self->{parent});
}

########################################################################
# Check whether a storage stack is empty.
#
# @return True if the stack is empty
##
sub isEmpty {
  my ($self) = assertNumArgs(1, @_);
  return !defined($self->{root});
}

########################################################################
# Get the device at the top of the stack.
#
# @return The top device on the stack or undef if the stack is empty
#         or branched
##
sub getTop {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->{root};
  while ($device) {
    my ($child, @rest) = $device->getChildren();
    if (@rest) {
      return undef;
    }

    if (!$child) {
      last;
    }

    $device = $child;
  }

  return $device;
}

########################################################################
# Create a volume group.
#
# @oparam storageDevice  The storage device on which to create the vg; if
#                        unspecified: the top of the stack will be used (unless
#                        that is ambiguous) unless the stack is empty in which
#                        case a raw device will be created and used
# @oparam name           The name for the new volume group
#
# @return a volume group
##
sub createVolumeGroup {
  my ($self, $storageDevice, $name) = assertMinMaxArgs(1, 3, @_);
  $name //= ($DEFAULT_LOGICAL_VOLUME_GROUP . $self->{nextVolumeGroup}++);
  $storageDevice
    //= ($self->isEmpty() ? $self->create('raw') : $self->getTop());
  assertType("Permabit::BlockDevice", $storageDevice);
  my %vgParams = (
                  storageDevice => $storageDevice,
                  volumeGroup   => $name,
                 );
  if (defined($self->{parent}{logicalVolumeExtentSize})) {
    $vgParams{physicalExtentSize} = $self->{parent}{logicalVolumeExtentSize};
  }

  return Permabit::VolumeGroup->new(%vgParams);
}

######################################################################
# Create an iSCSI target.
#
# @oparam storageDevice  The storage device on which to create the vg; if
#                        unspecified: the top of the stack will be used (unless
#                        that is ambiguous) unless the stack is empty in which
#                        case a raw device will be created and used
#
# @return an iSCSI target
##
sub createISCSITarget {
  my ($self, $storageDevice) = assertMinMaxArgs(1, 2, @_);
  $storageDevice
    //= ($self->isEmpty() ? $self->create('raw') : $self->getTop());
  assertType("Permabit::BlockDevice", $storageDevice);
  return $storageDevice->getISCSITarget();
}

########################################################################
# Get any devices in the stack which are of the specified type.
#
# @param  type   The class of device desired
# @oparam start  Start traversing the stack from the indicated device; defaults
#                to the bottom
#
# @return Any devices in the stack above the specified starting point which are
#         of the specified type
##
sub getDescendantsOfType {
  my ($self, $type, $start) = assertMinMaxArgs(2, 3, @_);
  my @result = ();
  my $device = $start // $self->{root};
  if (!defined($device)) {
    return @result;
  }

  if (!defined($start) && $device->isa($type)) {
    push(@result, $device);
  }

  push(@result, $device->getDescendantsOfType($type));

  return @result;
}

########################################################################
# Apply a function to a device and all of its descendants.
#
# @param  f          The function to apply to each device in the stack
# @oparam arguments  A hash of optional arguments:
#                      childSort: If present, used to determine the order in
#                                 which the function is applied to the children
#                                 of each device
#                      root:      The device from which to start, defaults to
#                                 the root of the stack
#                      topDown:   If true, the function will be applied to
#                                 parents before being applied to their
#                                 children; defaults to false
#
# @return A list of the results of applications
##
sub apply {
  my ($self, $f, %arguments) = assertMinArgs(2, @_);
  if ($self->isEmpty()) {
    return;
  }

  my $sort = $arguments{childSort} // sub { return @_ };

  my @devices = ();
  my @toScan  = ($arguments{root} || $self->{root});
  while (my $device = shift(@toScan)) {
    push(@devices, $device);
    push(@toScan, $sort->($device->getChildren()));
  }

  return map($f->($_), ($arguments{topDown} ? @devices : reverse(@devices)));
}

########################################################################
# Check all of the devices in the stack, starting from the top down.
##
sub check {
  my ($self) = assertNumArgs(1, @_);
  $self->apply(sub { return $_[0]->check(); });
}

########################################################################
# Wait for the dedupe operations to finish, and then read the current VDO
# statistics.  For stacks that don't have VDO running, this is a no-op.
#
# @return the VDO statistics or undef if there is no running VDO in the stack
##
sub getVDOStats {
  my ($self) = assertNumArgs(1, @_);
  my ($vdo) = $self->getDescendantsOfType('Permabit::BlockDevice::VDO');
  return (defined($vdo) ? $vdo->getVDOStats() : undef);
}

1;
