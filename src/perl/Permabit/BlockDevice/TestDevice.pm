##
# Perl object that represents a base class for test devices.
#
# $Id$
##
package Permabit::BlockDevice::TestDevice;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertMinMaxArgs
  assertNe
  assertNumArgs
);
use Permabit::Constants;
use Permabit::KernelModule;
use Permabit::ProcessUtils qw(delayFailures);

use base qw(Permabit::BlockDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple directory path containing the device node.
     deviceRootDir => "/dev/mapper",
     # @ple the version of the module
     moduleVersion => 1,
     # @ple the target type
     target        => undef,
     # @ple the kernel module
     _module       => undef,
    );
##

########################################################################
# Creates a C<Permabit::BlockDevice::TestDevice>.
#
# @param stack      The StorageStack which owns this device
# @param arguments  a hashref of additional properties
#
# @return a new C<Permabit::BlockDevice::TestDevice>
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
  $self->SUPER::configure($arguments);
  $self->{moduleName} //= "pbit$arguments->{typeName}";
  $self->{target}     //= $self->{typeName};
  if (!defined($self->getModuleSourceDir())) {
    my $sourceDir = $self->getMachine()->makeNfsSharePath("src/c++/devices");
    $self->setModuleSourceDir($sourceDir);
  }
}

########################################################################
# @inherit
##
sub setDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  $self->SUPER::setDeviceName($deviceName // $self->{typeName});
}

######################################################################
# Get module parameters
#
# @return The module parameters
##
sub getModuleParameters {
  my ($self) = assertNumArgs(1, @_);
  return (machine    => $self->getMachine(),
          modDir     => $self->getModuleSourceDir(),
          modName    => $self->getModuleName(),
          modVersion => $self->{moduleVersion});
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  if (!defined($self->{_module})) {
    my $module = Permabit::KernelModule->new($self->getModuleParameters());
    $module->load();
    $self->{_module} = $module;
  } else {
    $self->{_module}->reload();
  }

  $self->create();
  $self->addDeactivationStep(sub { $self->remove(); });
  $self->SUPER::activate();
}

######################################################################
# Create the device.
##
sub create {
  my ($self)  = assertNumArgs(1, @_);
  my $table   = $self->makeTableLine();
  my $command = "sudo dmsetup create $self->{deviceName} --table '$table'";
  $log->info($self->getMachineName() . ": $command");
  if ($self->sendCommand($command) == 0) {
    return;
  }

  my $machine = $self->getMachine();
  $log->error("Failure during $command");
  $log->error("stdout:\n" . $machine->getStdout());
  $log->error("stderr:\n" . $machine->getStderr());
  die("Cannot start device $self->{deviceName}");
}

########################################################################
# Construct the table line for this device.
##
sub makeTableLine {
  my ($self) = assertNumArgs(1, @_);
  return join(' ',
              $self->startTableLine(),
              $self->{deviceName},
              $self->{storageDevice}->getDevicePath());
}

######################################################################
# Construct the start of the table line which is common to all test devices.
##
sub startTableLine {
  my ($self) = assertNumArgs(1, @_);
  return join(' ',
              0,
              int($self->{storageDevice}->getSize() / $SECTOR_SIZE),
              $self->{target});
}

######################################################################
# Remove the device.
##
sub remove {
  my ($self) = assertNumArgs(1, @_);
  $self->getMachine()->dmsetupRemove($self->{deviceName});
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);
  delayFailures(sub { $self->SUPER::teardown(); },
                sub { $self->unloadModule(); });
}

######################################################################
# Unload the module if necessary.
##
sub unloadModule {
  my ($self) = assertNumArgs(1, @_);
  my $module = delete $self->{_module};
  if (defined($module)) {
    $module->unload();
  }
}

########################################################################
# Get the table entry for this device
#
# @return   the table entry string
##
sub getTable {
  my ($self)  = assertNumArgs(1, @_);
  my $devName = $self->getDeviceName();
  my $output  = $self->runOnHost("sudo dmsetup table $devName");
  $log->debug("table output: $output");
  return $output;
}

########################################################################
# Make the pathname to the sysfs file for a TestDevice
#
# @param name  The sysfs filename
#
# @return the full path to the sysfs file
##
sub makeSysfsPath {
  my ($self, $name) = assertNumArgs(2, @_);
  return join('/',
              '/sys',
              $self->getModuleName(),
              $self->getDeviceName(),
              $name);
}

1;
