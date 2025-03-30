##
# Perl object that represents a Linux block device
#
# $Id$
##
package Permabit::BlockDevice;

use strict;
use warnings FATAL => qw(all);
use Carp qw(
  cluck
  confess
  croak
);
use English qw(-no_match_vars);
use File::Basename;
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertMinArgs
  assertMinMaxArgs
  assertNe
  assertNotDefined
  assertNumArgs
  assertTrue
  assertType
);
use Permabit::BlkParse;
use Permabit::BlkParse qw($BLKPARSE_SUFFIX);
use Permabit::Constants qw(
  $MINUTE
  $SECTOR_SIZE
);
use Permabit::ProcessUtils qw(delayFailures);
use Permabit::Statistics::Disk;
use Permabit::SystemUtils qw(
  assertCommand
  runCommand
);
use Permabit::Utils qw(
  addToHash
  hashExtractor
  makeFullPath
  retryUntilTimeout
);

use overload q("") => \&toString,
             q(==) => \&equals;

use base qw(
  Permabit::BinaryFinder
  Permabit::Propertied
);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple devices which are stacked on top of this one
     children            => {},
     # @ple name of the device node
     deviceName          => undef,
     # @ple directory path containing the device node.  Can be overridden by a
     #      subclass, most frequently by /dev/mapper for device mapper devices.
     deviceRootDir       => "/dev",
     # @ple the ID of this device to its StorageStack
     id                  => undef,
     # @ple the kernel module name
     moduleName          => undef,
     # @ple the location of the module's source code
     moduleSourceDir     => undef,
     # @ple options need to mount a filesystem on the device
     mountOptions        => [],
     # @ple whether to set up a new device immediately
     setupOnCreation     => 1,
     # @ple the BlockDevice this device is build on top of
     storageDevice       => undef,
     # @ple pathname of the device as assigned by the user or test
     symbolicPath        => undef,
     # @ple the short name of the device type (for toString)
     typeName            => undef,
     # @ple whether to turn on tracing when starting the device
     tracing             => 0,
     # @ple the most recently generated blkparse output file
     _lastBlockParseFile => undef,
     # @ple boolean indicating if device should post-process block trace output
     #      after stopping block tracing
     _processBlockTrace  => 0,
    );
##

our %BLOCKDEVICE_INHERITED_PROPERTIES
  = (
     # Run directory of commands (needed indirectly by sub startTrace)
     runDir => undef,
    );

########################################################################
# Creates a C<Permabit::BlockDevice>. This method should only be called from
# StorageStack::create().
#
# @param stack      The StorageStack which owns this device
# @param arguments  A hashref of properties, which is cloned by
#                   StorageStack::create(), so it is safe to modify it.
#
# @return a new C<Permabit::BlockDevice>
##
sub new {
  my ($invocant, $stack, $arguments) = assertMinMaxArgs([{}], 2, 3, @_);
  my $class = ref($invocant) || $invocant;
  # We are only used as a base class.
  assertNe(__PACKAGE__, $class);

  my $self = bless { stack => $stack }, $class;
  eval {
    $self->configure($arguments);
  };
  if ($EVAL_ERROR) {
    confess($EVAL_ERROR);
  }

  $self->setDeviceName($self->{deviceName});

  assertType("Permabit::UserMachine", $self->getMachine());

  $self->{started} = 0;

  # register this device with its backing store
  if (defined($self->{storageDevice})) {
    $self->{storageDevice}->registerChild($self);
  }

  return $self;
}

########################################################################
# Configure a block device based on its parent and supplied arguments
#
# @param arguments  a hashref of additional properties
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2, @_);
  assertDefined($arguments->{typeName});

  my $inherited = $self->cloneClassHash("BLOCKDEVICE_INHERITED_PROPERTIES");
  my $inheritedKeys = [keys(%{$inherited})];
  addToHash($self,
            (%{$self->cloneClassHash("BLOCKDEVICE_PROPERTIES")},
             hashExtractor($inherited, $inheritedKeys),
             # overrides inherited properties only if defined
             $self->{stack}->getParentProperties($inheritedKeys),
             # overrides class properties and inherited properties
             %{$arguments},
            ));
  $self->{stack}->shareBinaryFinder($self);
  $self->checkStorageDevice();
}

########################################################################
# Check that the backing storage for this device is correct.
##
sub checkStorageDevice {
  my ($self) = assertNumArgs(1, @_);

  $self->{storageDevice} //= $self->{stack}->getTop();
  if (!defined($self->{storageDevice})) {
    $self->makeBackingDevice();
  }

  assertType("Permabit::BlockDevice", $self->{storageDevice});
}

########################################################################
# Make the backing device below this device.
##
sub makeBackingDevice {
  my ($self) = assertNumArgs(1, @_);
  # Use the stack directly here since the backing store isn't set up yet
  # and getMachine() defaults to using the backing store's machine.
  my $machine = $self->{stack}->getUserMachine();
  my $disks = $machine->selectDefaultRawDevices();
  if (scalar(@{$disks}) > 1) {
    $self->{storageDevice} = $self->{stack}->create("raid");
  } else {
    $self->{storageDevice} = $self->{stack}->create("raw");
  }
}

########################################################################
# Return the device ID assigned by the storage stack.
#
# @return The device ID
##
sub getDeviceID {
  my ($self) = assertNumArgs(1, @_);
  return $self->{id};
}

########################################################################
# Record the existence of a child device.
#
# @param device  The child device to register
##
sub registerChild {
  my ($self, $device) = assertNumArgs(2, @_);
  assertNotDefined($self->{children}{$device->{id}});
  $self->{children}{$device->getDeviceID()} = $device;
}

########################################################################
# Forget about a child device which has gone away.
#
# @param device  The child device to unregister
##
sub unregisterChild {
  my ($self, $device) = assertNumArgs(2, @_);
  delete $self->{children}{$device->getDeviceID()};
}

########################################################################
# Normalizes and sets the device name for the BlockDevice. Subclasses should
# override this if the default behavior is not correct.
##
sub setDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  assertDefined($deviceName);
  $self->{deviceName} = $self->_normalizedDeviceName($deviceName);
  $self->setSymbolicPath();
}

########################################################################
# Returns the normalized version of the passed device name.  Normalized means
# with any illegal characters from a device mapper perspective either
# eliminated or replaced.
#
# @param  deviceName  the name to normalize
#
# @return the normalized name
##
sub _normalizedDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  return $deviceName;
}

########################################################################
# Get the name of the device's kernel module
#
# @return the module name
##
sub getModuleName {
  my ($self) = assertNumArgs(1, @_);
  return $self->{moduleName};
}

########################################################################
# Get the location of the module's source code
#
# @return the module source dir
##
sub getModuleSourceDir {
  my ($self) = assertNumArgs(1, @_);
  return $self->{moduleSourceDir};
}

########################################################################
# Find the files necessary for full device operation.
##
sub resolveBinaries {
  my ($self) = assertNumArgs(1, @_);
}

########################################################################
# Set the relative location of the module's source code
#
# @param  relativeDir   the relative dir of the module's source
##
sub setModuleSourceDir {
  my ($self, $relativeDir) = assertNumArgs(2, @_);
  $self->{moduleSourceDir} = $relativeDir;
}

########################################################################
# Setup the device. Must be called after new
##
sub setup {
  my ($self) = assertNumArgs(1, @_);
  $self->resolveBinaries();
  $self->start();
}

########################################################################
# Start the device.
##
sub start {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{started}) {
    return;
  }

  $self->{started}             = 1;
  $self->{_deactivateSteps}    = [];
  $self->{_preDeactivateSteps} = [];

  $self->addPreDeactivationStep(sub { $self->stopBlockTrace(); }, 0);
  eval {
    $self->activate();
    if ($self->{tracing}) {
      $self->restartBlockTrace();
    }
    $self->postActivate();
  };
  if ($EVAL_ERROR) {
    my $error = $EVAL_ERROR;
    cluck($error);
    eval {
      $self->stop();
    };
    if ($EVAL_ERROR) {
      cluck("Error cleaning up from failed start: $EVAL_ERROR");
    }
    die($error);
  }
}

########################################################################
# Perform action to add device to lvm devices file.
##
sub addToDevicesFile {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevicePath();
  my $addDevCmd = "sudo lvmdevices -y --adddev $device";
  $self->runOnHost($addDevCmd);
  $self->addDeactivationStep(sub { $self->removeFromDevicesFile($device); })
}

########################################################################
# Perform action to remove device from lvm devices file.
# @param device The name of the device to remove.
##
sub removeFromDevicesFile {
  my ($self, $device) = assertNumArgs(2, @_);
  my $delDevCmd = "sudo lvmdevices -y --deldev $device";
  $self->runOnHost($delDevCmd);
}

########################################################################
# Perform actions to start a device.
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  $self->runOnHost("sudo chmod 666 " . $self->getDevicePath());
  $self->addToDevicesFile();
}

########################################################################
# Perform post-activation actions which prepare a device for testing.
##
sub postActivate {
  my ($self) = assertNumArgs(1, @_);
  # By default, nothing to do
}

######################################################################
# Add an unprepare step. An unprepare step is a coderef which takes no
# arguments. By making these closures instead of just coderefs which take the
# device as an argument, inheritance will continue to work on them.
#
# @param  step   The step to add
# @oparam fatal  Whether a failure of this step is fatal to the stop process,
#                defaults to true
##
sub addPreDeactivationStep {
  my ($self, $step, $fatal) = assertMinMaxArgs([1], 2, 3, @_);
  push(@{$self->{_preDeactivateSteps}}, [$step, $fatal]);
}

######################################################################
# Add a deactivation step. A deactivation step is a coderef which takes no
# arguments. By making these closures instead of just coderefs which take the
# device as an argument, inheritance will continue to work on them.
#
# @param  step   The step to add
# @oparam fatal  Whether a failure of this step is fatal to the stop process,
#                defaults to true
##
sub addDeactivationStep {
  my ($self, $step, $fatal) = assertMinMaxArgs([1], 2, 3, @_);
  push(@{$self->{_deactivateSteps}}, [$step, $fatal]);
}

########################################################################
# Stop the device
##
sub stop {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->{started}) {
    $log->info("not stopping $self since it already should be stopped");
    return;
  }

  $log->debug("stopping $self");
  my @errors = grep({ defined($_) }
                    map({ $self->_runSteps($_) }
                        qw(_preDeactivateSteps _deactivateSteps)));
  $self->{started} = 0;
  $log->debug("$self stopped");

  if (defined($errors[0])) {
    die($errors[0]);
  }
}

######################################################################
# Run code while the device is stopped, while remembering whether the
# device was started, and restarting it if so.
#
# @param code  The code to run while the device is stopped
##
sub runWhileStopped {
  my ($self, $code) = assertNumArgs(2, @_);
  my $wasStarted = $self->{started};
  if ($wasStarted) {
    $self->stop();
  }
  $code->();
  if ($wasStarted) {
    $self->start();
  }
}

######################################################################
# Move this device from one host to another, stopping it if necessary.
#
# @param newMachine  A RemoteMachine for the host to which to migrate.
##
sub migrate {
  my ($self, $newMachine) = assertNumArgs(2, @_);
  my $migrate = sub {
    $self->getStorageDevice()->migrate($newMachine);
  };
  $self->runWhileStopped($migrate);
}

########################################################################
# Reset the device to a clean state, and do any necessary recovery,
# for use when the device disappears outside the control of the stack
# (such as after an emergency reboot).
##
sub recover {
  my ($self) = assertNumArgs(1, @_);
  $self->{started} = 0;
  $self->start();
}

######################################################################
# Run the steps on a stack, handling errors appropriately.
#
# @param stepsField  Which field of $self contains the stack of steps to run,
#                    the field will be cleared
#
# @return The first non-fatal error any step encountered
##
sub _runSteps {
  my ($self, $stepsField) = assertNumArgs(2, @_);
  my $error;
  my $steps = delete $self->{$stepsField};
  while (my $step = pop(@{$steps})) {
    my ($action, $fatal) = @{$step};
    eval {
      $action->();
    };
    if ($EVAL_ERROR) {
      my $stepError = $EVAL_ERROR;
      if ($fatal) {
        confess($stepError);
      }

      cluck($stepError);
      if (!$error) {
        $error = $stepError;
      }
    }
  };

  return $error;
}

########################################################################
# Restart the device
##
sub restart {
  my ($self) = assertNumArgs(1, @_);
  $self->stop();
  $self->start();
}

########################################################################
# Resume the device
##
sub resume {
  my ($self) = assertNumArgs(1, @_);
  $self->runOnHost("sudo dmsetup resume " . $self->getDeviceName());
}

########################################################################
# Suspend the device
##
sub suspend {
  my ($self) = assertNumArgs(1, @_);
  $self->runOnHost("sudo dmsetup suspend " . $self->getDeviceName());
}

########################################################################
# Destroy the device. This method should only be called from StorageStack
# and should not be overridden.
##
sub destroy {
  my ($self) = assertNumArgs(1, @_);
  $log->debug("Destroying $self");
  $self->{stack}->assertDestroying($self->getDeviceID());
  $self->{_destroying} = 1;

  my $stopError;
  eval {
    $self->stop();
  };
  if ($EVAL_ERROR) {
    $stopError = $EVAL_ERROR;
    if ($self->{started}) {
      # stop failed
      confess($stopError);
    }

    # there was a non-fatal stop error
    cluck($stopError);
  }

  $self->teardown();
  if ($stopError) {
    die($stopError);
  }
}

########################################################################
# Teardown the device.
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);
  assertTrue($self->{_destroying});
  if (defined($self->{storageDevice})) {
    $self->{storageDevice}->unregisterChild($self);
  }
}

########################################################################
# Save logfiles and other device-specific files generated by tests with this
# device into the given directory.
#
# This method should be overridden by subclasses to save their own
# logfiles.
#
# @param saveDir        The directory to save the logfiles into
##
sub saveLogFiles {
  my ($self, $saveDir) = assertNumArgs(2, @_);
}

########################################################################
# Use /bin/dd to read from the device.  Takes arguments as key-value pairs.
#
# @oparam bs     Block size (passed using bs= to dd)
# @oparam count  Block count (passed using count= to dd)
# @oparam of     Output filename (passed using of= to dd)
# @oparam skip   The first block number to read from the device (passed using
#                skip= to dd)
##
sub ddRead {
  my ($self, %params) = assertMinArgs(1, @_);
  $self->getMachine()->dd(%params, if => $self->{symbolicPath});
}

########################################################################
# Use /bin/dd to write to the device.  Takes arguments as key-value pairs.
#
# @oparam bs     Block size (passed using bs= to dd)
# @oparam count  Block count (passed using count= to dd)
# @oparam if     Input filename (passed using if= to dd)
# @oparam oflag  Output flags (passed using oflag= to dd)
# @oparam seek   The first block number to write on the device (passed using
#                seek= to dd)
##
sub ddWrite {
  my ($self, %params) = assertMinArgs(1, @_);
  $self->getMachine()->dd(%params, of => $self->{symbolicPath});
}

########################################################################
# Get the major/minor device numbers for a given device
#
# @return the major and minor device numbers
##
sub getDeviceMajorMinor {
  my ($self) = assertNumArgs(1, @_);
  my $errno  = $self->sendCommand('ls -Hl ' . $self->getDevicePath());
  assertEqualNumeric(0, $errno);
  my @majorMinor = ($self->getMachine()->getStdout()
                    =~ m/^b[rwx-]+T?\.?\s+\d+[\s\w]+\s+(\d+),\s+(\d+)/);
  assertEqualNumeric(2, scalar(@majorMinor));
  return @majorMinor;
}

########################################################################
# Get the device name
#
# @return the device name
##
sub getDeviceName {
  my ($self) = assertNumArgs(1, @_);
  return $self->{deviceName};
}

########################################################################
# Get the device pathname
#
# @return the device pathname
##
sub getDevicePath {
  my ($self) = assertNumArgs(1, @_);
  assertDefined($self->{symbolicPath});

  # Resolve the device path name if it's a symlink because tools
  # that need to reference things in sysfs need to know the actual
  # device name. It's important to resolve the sym link every time
  # because on reboots, the device paths can change.
  return $self->_resolveSymlink($self->{symbolicPath});
}

########################################################################
# Get the resolved name of the device
#
# @return the resolved device name
##
sub getDeviceResolvedName {
  my ($self) = assertNumArgs(1, @_);
  return basename($self->getDevicePath());
}

########################################################################
# Get the disk statistics for the device
#
# @oparam diskstats The /proc/diskstats content to get the statistics from.
#                   If undefined, it will be gotten by "cat /proc/diskstats".
#
# @return the diskstats
##
sub getDiskStats {
  my ($self, $diskstats) = assertMinMaxArgs([undef], 1, 2, @_);
  $diskstats ||= $self->getMachine()->cat("/proc/diskstats");
  my @majorminor = $self->getDeviceMajorMinor();
  return Permabit::Statistics::Disk->newByMajorMinor($diskstats,
                                                     $majorminor[0],
                                                     $majorminor[1]);
}

########################################################################
# Get the machine containing the device.
#
# @return the machine
##
sub getMachine {
  my ($self) = assertNumArgs(1, @_);
  my $backing = $self->{storageDevice};
  if (!defined($backing)) {
    die("Can't find machine for $self");
  }

  return $backing->getMachine();
}

########################################################################
# Get the name of the machine containing the device.
#
# @return the machine name
##
sub getMachineName {
  my ($self) = assertNumArgs(1, @_);
  return $self->getMachine()->getName();
}

########################################################################
# Return additional options needed for mounting a filesystem on the device
#
# @return an array of mount options (or empty array)
##
sub getMountOptions {
  my ($self) = assertNumArgs(1, @_);
  return $self->{mountOptions};
}

########################################################################
# Return the machine where the device's backing store ultimately resides.
#
# @return the name of the machine hosting the storage
##
sub getStorageHost {
  my ($self) = assertNumArgs(1, @_);
  return $self->getStorageDevice()->getStorageHost();
}

########################################################################
# Get the pathname of a /sys/dev/block file associated with this device
#
# @param name  Name of the file
#
# @return full pathname
##
sub getSysDevBlockPath {
  my ($self, $name) = assertNumArgs(2, @_);
  my @majorminor = $self->getDeviceMajorMinor();
  return "/sys/dev/block/$majorminor[0]:$majorminor[1]/$name";
}

########################################################################
# Get the size of the device in bytes
#
# @return the size of the device in bytes
##
sub getSize {
  my ($self) = assertNumArgs(1, @_);
  my @majorminor = $self->getDeviceMajorMinor();
  my $sectors = $self->getMachine()->cat($self->getSysDevBlockPath("size"));
  return $SECTOR_SIZE * $sectors;
}

########################################################################
# Get the device symbolic path name
#
# @return the device symbolic path name
##
sub getSymbolicPath {
  my ($self) = assertNumArgs(1, @_);
  assertDefined($self->{symbolicPath});
  return $self->{symbolicPath};
}

########################################################################
# Check the kernel log for VDO related errors
##
sub checkForKernelLogErrors {
  my ($self) = assertNumArgs(1, @_);
  $self->getMachine()->checkForKernelLogErrors();
}

########################################################################
# Check for errors and other expectations of a device.
##
sub check {
  my ($self) = assertNumArgs(1, @_);
  $self->checkForKernelLogErrors();
}

########################################################################
# Fully resolve a possible symlink. If the device is not a symlink, just
# return the original path.
#
# @param  path  Device path
#
# @return the resolved device path.
#
# @croaks if the path is invalid.
##
sub _resolveSymlink {
  my ($self, $path) = assertNumArgs(2, @_);
  return $self->getMachine()->resolveSymlink($path);
}

########################################################################
# Generates and sets the symbolic path name for the BlockDevice.  This is
# the path of the device as assigned by the test, and is typically derived
# from the deviceName.  Subclasses should override this if the default
# behavior is not correct.
##
sub setSymbolicPath {
  my ($self) = assertNumArgs(1, @_);
  $self->{symbolicPath}
    = makeFullPath($self->{deviceRootDir}, $self->{deviceName});
}

########################################################################
# Check whether block trace is running against this device.
#
# @return true if this device is being traced
##
sub isTracing {
  my ($self) = assertNumArgs(1, @_);
  return ($self->{_blktrace}
          && Permabit::BlkTrace->isDeviceTraced($self->getMachineName(),
                                                $self->getDevicePath()));
}

########################################################################
# Return the file name of the last generated blkparse file.
#
# @return file name of last generated blkparse file; undef, if none.
##
sub _getLastBlockParseFile {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_lastBlockParseFile};
}

########################################################################
# Parses, via blkparse, the block tracing output for the device and sets the
# the name of the file containing the latest blkparse output.
#
# All blktrace output files without a corresponding blkparse output file are
# processed.
##
sub _parseBlockTrace {
  my ($self) = assertNumArgs(1, @_);
  assertDefined($self->{_blktrace}, "device is running blktrace itself");

  my $path = makeFullPath($self->{runDir}, join(".",
                                                $self->getDeviceName(),
                                                "*",
                                                "blktrace.0"));
  my @traceFiles = map({ $_ =~ s/\.blktrace\.0//; }
                       split("\n", $self->runOnHost("ls -1 $path")));

  # Remove the trace file for the current trace (last in array based on
  # timestamp in file name).
  pop(@traceFiles);

  $path
    = makeFullPath($self->{runDir},
                   join(".", $self->getDeviceName(), "*", $BLKPARSE_SUFFIX));
  my $result = runCommand($self->getMachineName(), "ls -1 $path");
  assertTrue(($result->{status} == 0) || ($result->{status} == 2),
             "ls of blkparse files success or no files found");
  my @parseFiles = map({ $_ =~ s/\.$BLKPARSE_SUFFIX//; }
                       split("\n", $result->{stdout}));

  # Process any trace files that haven't been.
  foreach my $traceFile (@traceFiles) {
    if (!grep { /$traceFile/ } @parseFiles) {
      my $args = {
                  baseFileName  => $traceFile,
                  doSudo        => 1,
                  host          => $self->{_blktrace}->getHost(),
                 };
      my $blkParse = Permabit::BlkParse->new($self, $args);
      $blkParse->run();
    }

    # Remove the trace files.
    $self->runOnHostIgnoreErrors("sudo rm -f $traceFile.blktrace.*");
  }

  my $traceFile = $self->{_blktrace}->getBaseFileName();
  my $args = {
              baseFileName  => $traceFile,
              doSudo        => 1,
              host          => $self->{_blktrace}->getHost(),
             };
  my $blkParse = Permabit::BlkParse->new($self, $args);
  $blkParse->run();
  $self->{_lastBlockParseFile} = $blkParse->getOutputFileName();
  $self->runOnHostIgnoreErrors("sudo rm -f $traceFile.blktrace.*");
}

########################################################################
# Restarts block tracing on a device.
#
# @oparam extra  extra arguments for starting blktrace
##
sub restartBlockTrace {
  my ($self, $extra) = assertMinMaxArgs([{}], 1, 2, @_);

  # If we should have a block trace running but we do not, start it.
  if ($self->{tracing}
      && !Permabit::BlkTrace->isDeviceTraced($self->getMachineName(),
                                             $self->getDevicePath())) {
    # Destroy the old ProcessServer object.
    delete $self->{_blktrace};

    # Start tracing.
    $self->startBlockTrace($extra);
  }
}

########################################################################
# Start block tracing a device.
#
# @oparam extra  extra arguments for starting blktrace
##
sub startBlockTrace {
  my ($self, $extra) = assertMinMaxArgs([{}], 1, 2, @_);
  if ($self->isTracing()) {
    return;
  }

  my $args = {
              coreFileName  => $self->getDeviceName(),
              devicePath    => $self->getDevicePath(),
              doSudo        => 1,
              host          => $self->getMachineName(),
              %$extra,
             };
  $self->{_blktrace} = Permabit::BlkTrace->new($self, $args);
  $self->{_blktrace}->run();
}

########################################################################
# Stop block tracing a device.
#
# @oparam processBlockTrace  if true, process the device's block trace output
##
sub stopBlockTrace {
  my ($self, $processBlockTrace) = assertMinMaxArgs([0], 1, 2, @_);
  my $trace = $self->{_blktrace};
  if (!defined($trace)) {
    return;
  }

  $trace->stop();
  if ($processBlockTrace || $self->{_processBlockTrace}) {
    $self->_parseBlockTrace();
  }

  # Destroy the ProcessServer object so that its initial shell goes
  # away and we can release the remote machine.
  delete $self->{_blktrace};
}

########################################################################
# Wait for the device to be ready (typically after a reboot)
##
sub waitForDeviceReady {
  my ($self) = assertNumArgs(1, @_);
  my $devName = $self->getSymbolicPath();
  # TODO Update the codebase to set the timeout via the machine type
  # as opposed to the setting here
  my $TIMEOUT = 5;
  $log->info("Waiting up to $TIMEOUT minutes for $devName to be usable");
  my $sub = sub {
    return ($self->sendCommand("test -b $devName") == 0);
  };
  retryUntilTimeout($sub, "$devName did not start", $TIMEOUT * $MINUTE);
}

########################################################################
# Get the backing store for a device.
#
# @return the backing store
##
sub getStorageDevice {
  my ($self) = assertNumArgs(1, @_);
  return $self->{storageDevice};
}

########################################################################
# Get any devices stacked immediately on a device.
#
# @return the list of children
##
sub getChildren {
  my ($self) = assertNumArgs(1, @_);
  return values(%{$self->{children}});
}

########################################################################
# Get any decendants of this device which are of a specified type.
#
# @param  type  The type of devices desired
#
# @return the list of descendants
##
sub getDescendantsOfType {
  my ($self, $type) = assertNumArgs(2, @_);
  my @result = ();
  foreach my $child ($self->getChildren()) {
    if ($child->isa($type)) {
      push(@result, $child);
    }

    push(@result, $child->getDescendantsOfType($type));
  }

  return @result;
}

########################################################################
# Get any ancestors of this device which are of a specified type.
#
# @param  type  The type of devices desired
#
# @return the list of ancestors
##
sub getAncestorsOfType {
  my ($self, $type) = assertNumArgs(2, @_);
  my @result = ();
  for (my $device = $self->getStorageDevice();
       defined($device);
       $device = $device->getStorageDevice()) {
    if ($device->isa($type)) {
      push(@result, $device);
    }
  }

  return @result;
}

########################################################################
# Overload default stringification.
##
sub toString {
  my ($self) = assertNumArgs(3, @_);
  my $name = $self->getDeviceName();
  my $path = ":$self->{symbolicPath}" // '';
  return "$name($self->{typeName})$path";
}

########################################################################
# Determine whether two BlockDevices are the same.
##
sub equals {
  my ($self, $other, $reversed) = assertNumArgs(3, @_);
  return ($other->isa(__PACKAGE__)
          && ($self->{stack} == $other->{stack})
          && ($self->getDeviceID() == $other->getDeviceID()));
}

######################################################################
# Run a list of commands on a BlockDevice's host.
#
# @param  commands   The command to run, or an array ref of commands
# @oparam logOutput  If true, log stdout of the command; if a true value
#                    other than '1', that value will be prepended to the
#                    logged output
#
# @return the output of the command(s)
##
sub runOnHost {
  my ($self, $commands, $logOutput) = assertMinMaxArgs([0], 2, 3, @_);
  my @result = map({ $_ = $self->_runCommandOnHost($_, $logOutput); }
                   ((ref($commands) eq 'ARRAY') ? @{$commands} : ($commands)));
  return (wantarray() ? @result : $result[0]);
}

######################################################################
# Run a single command on a BlockDevice's host.
#
# @param command    The command to run
# @param logOutput  If true, log stdout of the command; if a true value
#                   other than '1', that value will be prepended to the
#                   logged output
#
# @return The output of the command
##
sub _runCommandOnHost {
  my ($self, $command, $logOutput) = assertNumArgs(3, @_);
  my $machine = $self->getMachine();
  $machine->runSystemCmd($command);

  my $output = $machine->getStdout();
  if ($logOutput) {
    $log->info(($logOutput eq '1') ? $output : $logOutput . $output);
  }

  return $output;
}

######################################################################
# Run a command on a BlockDevice's host ignoring errors from the command.
#
# @param command  The command to run
#
# @return the output of the command
##
sub runOnHostIgnoreErrors {
  my ($self, $command) = assertNumArgs(2, @_);
  return runCommand($self->getMachineName(), $command)->{stdout};
}

######################################################################
# Run a command on the device's machine and return the error code.
#
# @param  command    The command to run
# @oparam logOutput  If true, log stdout from the command
#
# @return The return code of the command
##
sub sendCommand {
  my ($self, $command, $logOutput) = assertMinMaxArgs([0], 2, 3, @_);
  my $machine = $self->getMachine();
  my $result  = $machine->sendCommand($command);
  if ($logOutput) {
    $log->info($machine->getStdout());
  }

  return $result;
}

######################################################################
# Determine whether we expect good performance (especially latency)
# from this device.
##
sub supportsPerformanceMeasurement {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getMachine();
  return $self->{storageDevice}->supportsPerformanceMeasurement();
}
