##
# Perl object that represents a base class for test devices which are managed
# via a python script.
#
# $Id$
##
package Permabit::BlockDevice::TestDevice::Managed;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Path qw(mkpath);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertMinArgs
  assertMinMaxArgs
  assertNumArgs
  assertNe
  assertType
);
use Permabit::Constants;
use Permabit::KernelModule;
use Permabit::ProcessUtils qw(delayFailures);
use Permabit::Utils qw(makeFullPath);
use Permabit::SystemUtils qw(copyRemoteFilesAsRoot pythonCommand);

use base qw(Permabit::BlockDevice::TestDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# Creates a C<Permabit::BlockDevice::TestDevice::Managed>.
#
# @param stack      The StorageStack which owns this device
# @param arguments  a hashref of additional properties
#
# @return a new C<Permabit::BlockDevice::DeviceMapper>
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
sub setDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  $deviceName //= join('-',
                       $self->{typeName},
                       $self->getStorageDevice()->getDeviceName());
  $self->SUPER::setDeviceName($deviceName);
}

########################################################################
# @inherit
##
sub _normalizedDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  $deviceName = $self->SUPER::_normalizedDeviceName($deviceName);
  $deviceName =~ s/\//-/g;
  return $deviceName;
}

########################################################################
# @inherit
##
sub saveLogFiles {
  my ($self, $saveDir) = assertNumArgs(2, @_);
  my $host    = $self->getMachine()->getName();
  my $hostDir = makeFullPath($saveDir, $host);
  mkpath($hostDir);
  copyRemoteFilesAsRoot($host, "/", "var/lib/dkms/", $hostDir);
  $self->SUPER::saveLogFiles($saveDir);
}

########################################################################
# @inherit
##
sub create {
  my ($self) = assertNumArgs(1, @_);
  $self->sendDmdeviceCommand("create");
}

########################################################################
# @inherit
##
sub postActivate {
  my ($self) = assertNumArgs(1, @_);
  $self->addPreDeactivationStep(sub { $self->disable(); });
  $self->SUPER::postActivate();
}

########################################################################
# @inherit
##
sub remove {
  my ($self) = assertNumArgs(1, @_);
  $self->sendDmdeviceCommand("remove");
}

########################################################################
# Returns the options for a python command for creating/manipulating the
# device.
#
# @param  command   the command to be executed
#
# @return python    command options
##
sub getDmdeviceCommandOptions {
  my ($self, $command) = assertNumArgs(2, @_);
  my $deviceName = join("=", "--deviceName", $self->getDeviceName());
  my $serviceType = "";
  my $storageDevice = "";

  if ($command eq "create") {
    $serviceType = join("=", "--serviceType", $self->getDmdeviceServiceType());
    $storageDevice = join("=", "--storageDevice",
                          $self->getStorageDevice()->getSymbolicPath());
  }

  return join(" ", $deviceName, $serviceType, $storageDevice);
}

########################################################################
# Returns the environment for running a python command for
# creating/manipulating the device.
#
# @return the env command to prepend to the python command
##
sub getDmdeviceEnvironment {
  my ($self) = assertNumArgs(1, @_);
  return "env PYTHONPATH="
    . $self->getMachine()->makeNfsSharePath("src/python");
}

########################################################################
# Returns the dmdevice service type.
#
# @return dmdevice service type
##
sub getDmdeviceServiceType {
  my ($self) = assertNumArgs(1, @_);
  my $serviceType = $self->getModuleName();
  $serviceType =~ s/^pbit//;
  return $serviceType;
}

########################################################################
# Send a command to the device via the dmdevice service.
#
# Croaks if the devices is not managed by dmdevice.
#
# @param command  The command to execute (may contain additional options)
#
# @return the stdout from the command
##
sub sendDmdeviceCommand {
  my ($self, $command) = assertNumArgs(2, @_);
  my $dmdeviceOptions = $self->getDmdeviceCommandOptions($command);
  my $env = $self->getDmdeviceEnvironment();
  my $cmd = pythonCommand("$env $self->{dmdevice}",
                          join(" ", $command, $dmdeviceOptions), 1);
  return $self->runOnHost($cmd);
}

########################################################################
# Send the global enable command that all devices of this type understand.
##
sub enable {
  my ($self) = assertNumArgs(1, @_);
  $self->sendDmdeviceCommand("enable");
}

########################################################################
# Send the global disable command that all devices of this type understand.
##
sub disable {
  my ($self) = assertNumArgs(1, @_);
  $self->sendDmdeviceCommand("disable");
}

########################################################################
# Get the status of this device.
#
# @return the status string
##
sub getStatus {
  my ($self) = assertNumArgs(1, @_);
  my $output = $self->sendDmdeviceCommand("status");
  $log->debug("status output: $output");
  return $output;
}

########################################################################
# Get the path to a sysfs file in the /sys subtree for the module
#
# @param name  Relative path name to the sysfs file
#
# @return the absolute path name to the sysfs file
##
sub getSysModulePath {
  my ($self, $name) = assertNumArgs(2, @_);
  return makeFullPath("/sys", $self->getModuleName(), $name);
}

########################################################################
# Get the path to a sysfs file in the /sys subtree for this particular
# device.
#
# @param name  Relative path name to the sysfs file
#
# @return the absolute path name to the sysfs file
##
sub getSysDevicePath {
  my ($self, $name) = assertNumArgs(2, @_);
  my $devPath = makeFullPath($self->getDeviceName(), $name);
  return $self->getSysModulePath($devPath);
}

########################################################################
# Send a dmsetup message to the device
#
# @param message  The message string to send
##
sub sendMessage {
  my ($self, $message) = assertNumArgs(2, @_);
  my $devName          = $self->getDeviceName();
  $self->runOnHost("sudo dmsetup message $devName 0 $message");
}

########################################################################
# @inherit
##
sub restartBlockTrace {
  my ($self, $extra) = assertMinMaxArgs([{}], 1, 2, @_);
  my $args = {
              filter => 'notify',
              %$extra,
             };
  $self->SUPER::restartBlockTrace($args);
}

########################################################################
# @inherit
##
sub startBlockTrace {
  my ($self, $extra) = assertMinMaxArgs([{}], 1, 2, @_);
  my $args = {
              filter => 'notify',
              %$extra,
             };

  $self->SUPER::startBlockTrace($args);
}

1;
