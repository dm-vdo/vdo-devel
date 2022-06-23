##
# This is an AsyncTask that will read sysfs until it fails, then do so until
# it succeeds. This is intended to be run while the test causes a stop/start
# cycle, attempting to provoke VDO-4155, where reading sysfs when VDO wasn't
# fully operational could cause crashes.
#
# $Id$
##
package Permabit::VDOTask::ReadSysfsWhenNotRunning;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Assertions qw(assertNumArgs);

use base qw(Permabit::VDOTask);

###############################################################################
# Set up a new Permabit::VDOTask::ReadSysfsWhenNotRunning.
#
# @param device  The vdo device.
# @param started True to check for started device. False to check for stopped.
#
# @return the new object
##
sub new {
  my ($invocant, $device, $started) = assertNumArgs(3, @_);
  my $self = $invocant->SUPER::new();
  $self->{device} = $device;
  $self->{started} = $started;
  $self->useDevice($device);
  return $self;
}

###############################################################################
# @inherit
##
sub taskCode {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->{device}->getMachine();
  # Since we've got rid of the pool name in the table line, we're now using the
  # device's major:minor value. Problem is when the device is no longer there,
  # we are unable to get that value. This could be true even at the point the
  # task starts. So we are gonna use a wildcard here to cat every vdo on the
  # system (which is only one for this test).
  my $sysfsPath = "/sys/block/*/vdo/compressing";
  if ($self->{started}) {
    # Spin reading the sysfs file till it exists (device has started again).
    $machine->executeCommand("while :; do cat $sysfsPath 2>/dev/null"
			     . " && break; done;");
  } else {
    # Spin reading the sysfs file till it ceases to exist (device has stopped).
    $machine->executeCommand("while :; do cat $sysfsPath > /dev/null"
			     . " || break; done;");
  }
  return undef;
}

1
