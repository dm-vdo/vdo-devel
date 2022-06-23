##
# A C<BlockDevice> which must be at the bottom of the portion of the device
# stack which is managed via a C<Permabit::StorageStack>.
#
# $Id$
##
package Permabit::BlockDevice::Bottom;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertMinMaxArgs
  assertNe
  assertNotDefined
  assertNumArgs
);
use Permabit::SystemUtils qw(getScamVar);

use base qw(Permabit::BlockDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple Permabit::UserMachine the device is on
     machine => undef,
    );
##

########################################################################
# Creates a C<Permabit::BlockDevice::Bottom>.
#
# @param stack      The StorageStack which owns this device
# @param arguments  a hashref of additional properties
#
# @return a new C<Permabit::BlockDevice::Bottom>
##
sub new {
  my ($invocant, $stack, $arguments) = assertMinMaxArgs([{}], 2, 3, @_);
  my $class = ref($invocant) || $invocant;
  # We are only used as a base class.
  assertNe(__PACKAGE__, $class);
  return $class->SUPER::new($stack, $arguments);
}

########################################################################
# @inherit
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2, @_);
  $arguments->{machine} //= $self->{stack}->getUserMachine();
  $self->SUPER::configure($arguments);
}

########################################################################
# @inherit
##
sub checkStorageDevice {
  my ($self) = assertNumArgs(1, @_);
  assertNotDefined($self->{storageDevice});
}

########################################################################
# @inherit
##
sub makeBackingDevice {
  my ($self) = assertNumArgs(1, @_);
  # Here to protect Bottom devices in the case this function is
  # accidentally called. Usually it won't be due to the code above in
  # checkStorageDevice blocking the normal call to makeBackingDevice
  # in BlockDevice.pm.
  confess(ref($self) . " can't have backing device. It is a bottom device.");
}

########################################################################
# @inherit
##
sub getMachine {
  my ($self) = assertNumArgs(1, @_);
  return $self->{machine};
}

########################################################################
# @inherit
##
sub getStorageHost {
  my ($self) = assertNumArgs(1, @_);
  return $self->getMachine()->getName();
}

########################################################################
# @inherit
##
sub migrate {
  my ($self, $newMachine) = assertMinMaxArgs(1, 2, @_);
  die("$self can't be migrated.");
}

########################################################################
# @inherit
##
sub supportsPerformanceMeasurement {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getMachine();
  # Virtual I/O may not have reliable performance.
  if ($machine->isVirtual()) {
    return 0;
  }
  # Some Beaker machines have disks with poor latency.
  if (!!getScamVar($machine->getName(), "BEAKER")) {
    return 0;
  }
  # Non-virtualized lab machines should be okay.
  return 1;
}

1;
