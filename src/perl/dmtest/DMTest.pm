##
# Base class for DMTests.
#
# $Id$
##
package DMTest;

use strict;
use warnings FATAL => qw(all);
use Carp qw(confess croak);
use Cwd qw(cwd);
use English qw(-no_match_vars);
use File::Path;
use List::Util qw(max);
use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertFalse
  assertGTNumeric
  assertMinArgs
  assertMinMaxArgs
  assertNotDefined
  assertNumArgs
  assertTrue
  assertType
);
use Permabit::BlockDevice;
use Permabit::CommandString::DMTest;
use Permabit::Constants;
use Permabit::KernelModule;
use Permabit::KernelUtils qw(
  setupKernelMemoryLimiting
  removeKernelMemoryLimiting
  setupKmemleak
  removeKmemleak
  setupRawhideKernel
  removeRawhideKernel
);
use Permabit::PlatformUtils qw(getDistroInfo);
use Permabit::RSVPer;
use Permabit::StorageStack;
use Permabit::SystemUtils qw(
  assertCommand
  assertSystem
  createRemoteFile
  pythonCommand
);
use Permabit::UserMachine;
use Permabit::UserModule;
use Permabit::Utils qw(
  makeFullPath
  parseBytes
  secondsToMS
  sizeToText
);
use Permabit::Version qw($VDO_MARKETING_VERSION $VDO_MODNAME $VDO_VERSION);
use Permabit::VolumeGroup;

use base qw(Permabit::Testcase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);


my $VDO_USER_MODNAME = "vdo";

our %PROPERTIES
  = (
     # @ple what class of machine to run the test on
     clientClass            => "FARM,RAWHIDE",
     # @ple Label for the client host
     clientLabel            => "dmtest",
     # @ple The names of the machines to be used for clients.  If not
     #      specified, numClients machines will be reserved.
     clientNames            => undef,
     # @ple Optional default host to use for the test
     defaultHost            => undef,
     # @ple The directory to put dmtest in
     dmtestDir              => undef,
     # @ple the regexp to choose for running tests
     dmtestName             => ".*",
     # @ple where to get dmtest-python from
     dmtestRepo             => "https://github.com/dm-vdo/dmtest-python.git",
     # @ple use one client machine
     numClients             => 1,
     # @ple Reference to the list of pre-reserved hosts passed in to the test.
     prereservedHosts       => [],
     # @ple Ask rsvpd to randomize its list of available hosts before selecting
     randomizeReservations  => 1,
     # @ple the max number of hung task warnings to report in kern.log
     hungTaskWarnings       => 25,
     # @ple Turn on the kernel memory allocation checker
     kmemleak               => 0,
     # @ple use one client machine
     numClients             => 1,
     # @ple The directory to find python libraries in
     pythonLibDir           => undef,
     # @ple Ask rsvpd to randomize its list of available hosts before selecting
     randomizeReservations  => 1,
     # @ple The directory to put generated datasets in
     scratchDir             => undef,
     # @ple Suppress clean up of the test machines if one of these named error
     #      types occurs.
     suppressCleanupOnError => ["Verify"],
     # @ple The directory to put the user tool binaries in
     userBinaryDir          => undef,
     # @ple Use the dmlinux src rpm for testing.
     useUpstreamKernel      => 0,
    );

my @SRPM_NAMES
  = (
     "src/srpms/kmod-kvdo-$VDO_VERSION-*.src.rpm",
     "src/srpms/vdo-$VDO_VERSION-*.src.rpm",
    );

my @RPM_NAMES
  = (
     "archive/kmod-kvdo-$VDO_VERSION-1.*.rpm",
     "archive/vdo-$VDO_VERSION-1.*.rpm",
     "archive/vdo-support-$VDO_VERSION-1.*.rpm",
    );

my @UPSTREAM_NAMES
  = (
     "src/packaging/dmlinux/build/SRPMS/kmod-kvdo-$VDO_VERSION-*.src.rpm",
     "src/srpms/vdo-$VDO_VERSION-*.src.rpm",
    );

my @SHARED_PYTHON_FILES
  = (
     "dmtest",
     "src/scripts/make-config",
     );

my @SHARED_FILES
  = (
     "src/c++/tools/fsync",
     "src/python/vdo/__init__.py",
     "src/python/vdo/dmdevice",
     "src/python/vdo/utils",
    );

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->{prereservedHosts} =
    $self->canonicalizeHostnames($self->{clientNames});
  $self->reserveHosts();

  $self->SUPER::set_up();

  $self->{scratchDir} = makeFullPath($self->{workDir}, 'scratch');
  $self->{userBinaryDir} = makeFullPath($self->{runDir}, 'executables');
  $self->{dmtestDir} = makeFullPath($self->{userBinaryDir}, 'dmtest');

  if (!defined($self->{pythonLibDir})) {
    $self->{pythonLibDir} = $self->{binaryDir} . "/pythonlibs";
  }

  # Decide what kind of default bottom device to make.
  my $stack = $self->getStorageStack();
  my $disks = $stack->getUserMachine()->selectDefaultRawDevices();
  my $deviceType = "raw";
  if (scalar(@{$disks}) > 1) {
    $deviceType = "raid";
  }

  foreach my $type (reverse(split('-', $deviceType))) {
    $self->createTestDevice($type);
  }

  $self->installModules();
  $self->installDMTest();

  foreach my $path (@SHARED_PYTHON_FILES) {
    my $dest = "$self->{dmtestDir}/$path";
    # Note that due to races between concurrent tests, multiple ln
    # processes could collide and cause errors.
    $self->runOnHost("test -e $dest");
  }

  my $machine = $stack->getUserMachine();
  my $storage = $self->getDevice()->getDevicePath();
  my $config = "$self->{dmtestDir}/config.toml";
  $self->runOnHost($self->{dmtestDir} . "/src/scripts/make-config"
                   . " --dataDevice=$storage"
                   . " --metadataDevice=$storage"
                   . " --disableDeviceCheck"
                   . " --outputFile=$config");

  $self->runOnHost("test -e $config");
}

########################################################################
# Create a test device
#
# @param deviceType  Type of device to create
# @oparam extra      Extra arguments to be passed to the new() method.
#
# @return the created Permabit::BlockDevice
##
sub createTestDevice {
  my ($self, $deviceType, %extra) = assertMinArgs(2, @_);
  return $self->getStorageStack()->create($deviceType, { %extra });
}

########################################################################
# Notify the test that a device is to be destroyed. This method is
# set as the device destroy hook of the storage stack during teardown.
##
sub destroyDevice {
  my ($self, $device) = assertNumArgs(2, @_);
  $self->runTearDownStep(sub { $device->stop() });
}

########################################################################
# Destroy a test device.
#
# @param device  The device to destroy
##
sub destroyTestDevice {
  my ($self, $device) = assertNumArgs(2, @_);
  $self->getStorageStack()->destroy($device);
}

########################################################################
# Get the default host to use for the test
#
# @return The hostname of the default host
##
sub getDefaultHost {
  my ($self) = assertNumArgs(1, @_);
  return $self->{defaultHost} // $self->{clientNames}[0];
}

########################################################################
# Gets a device from the storage stack. By default, returns the top device in
# the stack if the stack is unbranched (branched stacks are not quite supported
# yet). If an optional type is supplied, the upper-most device of that type is
# returned.
#
# @oparam The type of device desired
#
# @return the Permabit::BlockDevice
##
sub getDevice {
  my ($self, $type) = assertMinMaxArgs(1, 2, @_);
  if (defined($type)) {
    my @devices = $self->getStorageStack()->getDescendantsOfType($type);
    return pop(@devices);
  }

  return $self->getStorageStack()->getTop();
}

########################################################################
# Get the storage stack, constructing it if necessary.
#
# @return The storge stack
##
sub getStorageStack {
  my ($self) = assertNumArgs(1, @_);
  if (!defined($self->{_storageStack})) {
    $self->{_storageStack} = Permabit::StorageStack->new($self);
  }
  return $self->{_storageStack};
}

########################################################################
# @inherit
##
sub getTestHosts {
  my ($self) = assertNumArgs(1, @_);
  return (@{$self->{prereservedHosts}},
          $self->SUPER::getTestHosts(),
          $self->getRSVPer()->getReservedHosts());
}

########################################################################
# Gets a UserMachine that is used for running tests. If a machine doesn't
# exist then one will be created and cached.
#
# @oparam name  The hostname.  The default is to assume the the test uses
#               a single client machine.
#
# @return a Permabit::UserMachine for the first clientName host
##
sub getUserMachine {
  my ($self, $name) = assertMinMaxArgs([undef], 1, 2, @_);
  $name ||= $self->getDefaultHost();
  assertDefined($name);
  if (!defined($self->{_machines}{$name})) {
    my %params = (
                  hostname         => $name,
                  hungTaskWarnings => $self->{hungTaskWarnings},
                  nfsShareDir      => $self->{nfsShareDir},
                  scratchDir       => $self->{scratchDir},
                  workDir          => $self->{workDir},
                  userBinaryDir    => $self->{userBinaryDir},
                 );
    $self->{_machines}{$name} = Permabit::UserMachine->new(%params);
  }
  return $self->{_machines}{$name};
}

########################################################################
# Get the name of the machine containing the device.
#
# @return the machine name
##
sub getUserMachineName {
  my ($self) = assertNumArgs(1, @_);
  return $self->getUserMachine()->getName();
}

########################################################################
# Install all things needed for DMTest to run that can't be setup
# during machine image work.
##
sub installDMTest {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getUserMachine();
  $machine->assertExecuteCommand("git clone "
                                 . $self->{dmtestRepo} . " "
                                 . $self->{dmtestDir});
  $self->{_dmtestInstalled} = 1;
}

########################################################################
# Install the kernel and user tools modules
##
sub installModules {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getUserMachine();
  my $machineName = $self->getUserMachineName();
  my $moduleName = $VDO_MODNAME;
  my $version = $VDO_MARKETING_VERSION;
  if ($self->{_modules}{$machineName}) {
    if ($self->{_modules}{$machineName}{$moduleName}{modVersion} eq $version) {
      $log->info("Module $moduleName $version already installed"
                 . "on $machineName");
      return;
    } else {
      # We need a different version of the module, so remove the existing one.
      $self->uninstallModule($machineName);
    }
  }

  # Build the kernel and user tool modules from source.
  $log->info("Installing module $moduleName $version on $machineName");
  my $module
    = Permabit::KernelModule->new(
                                  machine         => $machine,
                                  modDir          => $self->{binaryDir},
                                  modFileName     => "kmod-$moduleName",
                                  modName         => $moduleName,
                                  modVersion      => $version,
                                  useDistribution => $self->{useDistribution},
                                 );
  $module->load();
  $self->{_modules}{$machineName}{$moduleName} = $module;

  $log->info("Installing module $VDO_USER_MODNAME $version on $machineName");
  my $userModule
    = Permabit::UserModule->new(
                                machine         => $machine,
                                modDir          => $self->{binaryDir},
                                modName         => $VDO_USER_MODNAME,
                                modVersion      => $version,
                                useDistribution => $self->{useDistribution},
                               );
  $userModule->load();
  $self->{_modules}{$machineName}{$VDO_USER_MODNAME} = $userModule;

  # Because some errors like kernel-module assertion failures may go
  # unrecognized until cleanup, after we've torn down the devices and
  # discarded the module, we save a copy of the module now.
  my $modulePath = $self->runOnHost("sudo modinfo $moduleName -F filename");
  chomp($modulePath);
  $self->runOnHost("sudo cp $modulePath $self->{runDir}");
  # VDO-5320: lvm uses the wrong module name on RHEL 9, so make a symlink to
  # fool it.
  if (getDistroInfo($machineName) eq "RHEL9") {
    my $moduleBase = $modulePath;
    $moduleBase =~ s|/extra.*||;
    $self->{_dmSymlink}{$machineName} = "$moduleBase/weak-updates/dm-vdo.ko";
    $self->runOnHost("sudo ln -snf $moduleBase/extra/kmod-kvdo/vdo/kvdo.ko "
                     . $self->{_dmSymlink}{$machineName});
    $self->runOnHost("sudo depmod -a");
  }
}

########################################################################
# @inherit
##
sub listSharedFiles {
  my ($self) = assertNumArgs(1, @_);
  my @files = ($self->SUPER::listSharedFiles(), @SHARED_FILES);
  if ($self->{useUpstreamKernel}) {
    return (@files, @UPSTREAM_NAMES);
  } elsif ($self->{useDistribution}) {
    return (@files, @RPM_NAMES);
  } else {
    return (@files, @SRPM_NAMES);
  }
}

########################################################################
# TODO
##
sub listTests {
  my ($self, $filter) = assertNumArgs(2, @_);
  return $self->runDMTestCommand("list", { dmtestName => $filter });
}

########################################################################
# TODO
##
sub listTestLogs {
  my ($self, $filter) = assertNumArgs(2, @_);
  return $self->runDMTestCommand("log", { dmtestName => $filter });
}

########################################################################
# Make a dmtest command with arguments.
#
# @param  command    the command to run
# @oparam extraArgs  additional arguments to the command
##
sub makeDMTestCommand {
  my ($self, $command, $extraArgs) = assertMinMaxArgs([{}], 2, 3, @_);
  my $dmtest = makeFullPath($self->{dmtestDir}, 'dmtest');
  my $args = {
              binary        => $dmtest,
              command       => $command,
              dmtestDir     => $self->{dmtestDir},
              doSudo        => 1,
              logfile       => $self->{dmtestLogFile},
              verbose       => 1,
              %$extraArgs,
             };
  return $self->makeDMTestCommandString($args);
}

########################################################################
# Hook/factory method to construct a dmtest command string.
#
# @oparam args  the arguments to the command
#
# @return a new C<Permabit::CommandString::DMTest>
##
sub makeDMTestCommandString {
  my ($self, $args) = assertNumArgs(2, @_);
  return Permabit::CommandString::DMTest->new($self, $args);
}

########################################################################
# Reserve the hosts needed for the test.
#
# May be overridden by subclasses.
##
sub reserveHosts {
  my ($self) = assertNumArgs(1, @_);
  $self->reserveHostGroups("client");
}

########################################################################
# Run a dmtest command.
#
# @param  command    the command to run
# @oparam extraArgs  additional arguments to the command
# @oparam okToFail   do or do not allow failure
##
sub runDMTestCommand {
  my ($self, $command, $extraArgs, $okToFail)
    = assertMinMaxArgs([{}, 0], 2, 4, @_);

  my $machine = $self->getUserMachine();
  my $dmTestCommand = $self->makeDMTestCommand($command, $extraArgs);
  if ($okToFail) {
    $machine->executeCommand("($dmTestCommand)");
  } else {
    $machine->assertExecuteCommand("($dmTestCommand)");
  }
  return $machine->getStdout();
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
  my $machine = $self->getUserMachine();
  $machine->runSystemCmd($command);

  my $output = $machine->getStdout();
  if ($logOutput) {
    $log->info(($logOutput eq '1') ? $output : $logOutput . $output);
  }

  return $output;
}

######################################################################
# Run a list of commands on a host.
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

########################################################################
# TODO
##
sub runTests {
  my ($self, $filter) = assertNumArgs(2, @_);
  return $self->runDMTestCommand("run", { dmtestName => $filter });
}

########################################################################
# Save logfiles related to the VDO kernel module generated by this test into
# the given directory.  This method should be overridden by subclasses to save
# their own logfiles related to the kernel module.
#
# @param saveDir  The directory to save the logfiles into
##
sub saveKernelLogFiles {
  my ($self, $saveDir) = assertNumArgs(2, @_);

  $log->info("Saving kernel modules to $saveDir");

  my $stack = $self->getStorageStack();
  if ($stack->isEmpty()) {
    $log->info("No modules saved since no devices are defined");
    return;
  }

  my @devices = reverse($stack->getDescendantsOfType('UNIVERSAL'));
  foreach my $device (@devices) {
    my $host = $device->getMachineName();
    $log->info("Saving kernel modules for client host $host");
    $self->runTearDownStep(sub { $device->saveLogFiles($saveDir); });
  }
}

########################################################################
# Uninstall the kernel and user tools modules
##
sub uninstallModules {
  my ($self, $machineName) = assertMinMaxArgs(1, 2, @_);
  $machineName //= $self->getUserMachineName();
  $log->info("Uninstalling modules from $machineName");
  if (defined($self->{_modules}{$machineName})) {
    # VDO-5320: Remove workaround symlink.
    if (defined($self->{_dmSymlink}{$machineName})) {
      $self->runOnHost("sudo rm -f $self->{_dmSymlink}{$machineName}");
      delete($self->{_dmSymlink}{$machineName});
    }
  }

  # Remove any installed modules
  foreach my $moduleName ($VDO_USER_MODNAME, $VDO_MODNAME) {
    if (defined($self->{_modules}{$machineName}{$moduleName})) {
      $self->{_modules}{$machineName}{$moduleName}->unload();
      delete($self->{_modules}{$machineName}{$moduleName});
    }
  }

  # Memory leaks are not logged until the module is uninstalled
  $self->getUserMachine()->checkForKernelLogErrors();
}

########################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);

  # Save kernel module that was compiled on the test host.
  #
  # This needs to occur during the tear down at the VDOTest inheritance /
  # call-hierarchy level due to the order of calls in overridden functions
  # between superclass and subclass (it's in bottom-up order from subclass
  # to superclass).  Waiting for the saveAllLogFiles() method call inside
  # the SUPER::tear_down() method does not work since the VDO device is
  # destroyed before then.
  if ($self->shouldSaveLogs()) {
    my $saveDir = $self->getDefaultLogFileDirectory();
    $self->saveKernelLogFiles($saveDir);
  }

  $self->uninstallModules();

  if (!$self->{suppressCleanup}) {
    # Tear down devices (includes any VDO device that was created).
    my $stack = $self->getStorageStack();
    $stack->setDeviceDestroyHook(sub { $self->destroyDevice(@_) });
    $stack->setTeardownWrapper(sub { $self->runTearDownStep(@_) });
    $stack->destroyAll();
    delete $self->{_storageStack};

    # Close the UserMachines because we are about to release our RSVP
    # reservations.
    map { $_->closeForRelease() } values(%{$self->{_machines}});
    delete $self->{_machines};
  }

  $self->SUPER::tear_down();
}

1;
