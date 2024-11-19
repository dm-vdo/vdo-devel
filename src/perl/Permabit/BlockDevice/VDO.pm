##
# Perl object that represents a kernel VDO device managed by vdo manager script
#
# $Id$
##
package Permabit::BlockDevice::VDO;

use strict;
use warnings FATAL => qw(all);
use Carp qw(confess croak);
use English qw(-no_match_vars);
use File::Basename;
use File::Path qw(mkpath);
use JSON;
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertEqualNumeric
  assertMinArgs
  assertMinMaxArgs
  assertLENumeric
  assertNe
  assertNumArgs
  assertOptionalArgs
  assertTrue
);
use Permabit::BlkTrace;
use Permabit::CommandString::VDOStats;
use Permabit::Constants;
use Permabit::KernelModule;
use Permabit::LabUtils qw(isVirtualMachine);
use Permabit::PlatformUtils qw(getDistroInfo);
use Permabit::ProcessUtils qw(delayFailures);
use Permabit::Statistics::VDO;
use Permabit::SystemUtils qw(
  copyRemoteFilesAsRoot
  runCommand
);
use Permabit::UserModule;
use Permabit::Utils qw(
  makeFullPath
  retryUntilTimeout
  secondsToMS
  timeToText
  yamlStringToHash
);
use Permabit::Version qw(
  $VDO_MARKETING_VERSION
  $VDO_MODNAME
);

use base qw(Permabit::BlockDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $VDO_WARMUP               = "src/c++/vdo/bin/vdoWarmup";
my $VDO_INITIALIZE_BLOCK_MAP = "src/c++/vdo/bin/vdoInitializeBlockMap";

my $VDO_USER_MODNAME = "vdo";
my $VDO_UPSTREAM_MODNAME = "dm-vdo";

# The keys of this hash are the names of the histogram that records the
# statistic, and the values are used in the error message if any request
# exceeds the maximum allowed latency.
my %LATENCY_CHECKS = (
                      "acknowledge_discard" => "VDO discard",
                      "acknowledge_read"    => "VDO read",
                      "acknowledge_write"   => "VDO write",
                      "bio_read"            => "bio read",
                      "bio_write"           => "bio write",
                     );

########################################################################
# @paramList{new}
#
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple Flag whether the VDO is converted until VDOSTORY-275 is complete
     converted               => 0,
     # @ple name of the device node
     deviceName              => "vdo0",
     # @ple directory path containing the device node.
     deviceRootDir           => "/dev/mapper",
     # workaround for VDO-5320
     _dmSymlink              => {},
     # @ple whether to wait for the indexer to come up at start
     expectIndexer           => 1,
     # @ple whether to expect VDO and the index to rebuild
     expectRebuild           => 0,
     # @ple the path to the VDO installer script
     installer               => undef,
     # VDO physical metadata size (in bytes)
     metadataSize            => undef,
     # Hash of installed VDO modules
     _modules                => {},
     # VDO module name
     moduleName              => $VDO_MODNAME,
     # VDO module version
     moduleVersion           => undef,
     # Whether readable storage is enabled, for cleanup
     _readableStorageEnabled => 0,
     # Whether this VDO may be stacked on another VDO
     stackable               => 0,
     # The name of the created VDO device. The distinction
     # is needed in order to support LVMManaged which is a
     # hybrid device on both a VDO and a volume on top of it
     vdoDeviceName           => undef,
     # The path to the created VDO device. The distinction
     # is needed in order to support LVMManaged which is a
     # hybrid device on both a VDO and a volume on top of it
     vdoSymbolicPath         => undef,
     # Whether writable storage is enabled, for cleanup
     _writableStorageEnabled => 0,
    );
##

# These are the properties inherited from the testcase.  Note that testcase
# base classes like VDOTest directly copy this hash into its own properties.
# The defaults here are then used.
our %BLOCKDEVICE_INHERITED_PROPERTIES
  = (
     # true means fill the index
     albFill                   => 0,
     # The number of bio acknowledgement threads to use
     bioAckThreadCount         => undef,
     # The number of bio submission threads to use
     bioThreadCount            => undef,
     # How often to rotate between bio submission work queues
     bioThreadRotationInterval => undef,
     # The amount of memory allocated for cached block map pages
     blockMapCacheSize         => undef,
     # The block map period
     blockMapPeriod            => undef,
     # the physical block size of the VDO device
     blockSize                 => 4 * $KB,
     # The class of machines this test is running on
     clientClass               => undef,
     # The type of compression to use, plus any arguments
     compressionType           => undef,
     # The number of "CPU" (hashing etc) threads to use
     cpuThreadCount            => undef,
     # whether to start or skip the indexer
     disableAlbireo            => 0,
     # emulate a 512 byte block device
     emulate512Enabled         => undef,
     # whether to enable compression in module
     enableCompression         => 0,
     # whether to enable deduplication in module
     enableDeduplication       => 1,
     # @ple whether to format using vdoformat or through the kernel.
     formatInKernel            => 0,
     # Number of hash lock threads/zones to use
     hashZoneThreadCount       => 1,
     # Maximum VDO I/O request latency in seconds
     latencyLimit              => 30,
     # VDO logical size (in bytes)
     logicalSize               => 20 * $GB,
     # Number of logical threads/zones to use
     logicalThreadCount        => 3,
     # Memory size of albireo index
     memorySize                => 0.25,
     # @ple VDO settings to apply at next restart
     pendingSettings           => {},
     # @ple VDO physical size (in bytes)
     physicalSize              => undef,
     # Number of physical threads/zones to use
     physicalThreadCount       => 2,
     # test kvdo queue priorities
     queuePriorities           => undef,
     # The directory to save logs in
     runDir                    => undef,
     # Only used by upgrade device.
     setupVersion              => undef,
     # Whether to use a sparse index
     sparse                    => 0,
     # The directory to put generated datasets in
     scratchDir                => undef,
     # The number of bits in the VDO slab
     slabBits                  => undef,
     # Whether the test is loading a released RPM
     useDistribution           => 0,
     # The directory to put the user tool binaries in
     userBinaryDir             => undef,
     # Whether to use dm-vdo
     useUpstreamModule         => 0,
     # UUID for VDO volume.
     uuid                      => undef,
     # Bitmask of CPUs on which to run VDO threads
     vdoAffinity               => undef,
     # Comma-separated list of CPUs on which to run VDO threads
     vdoAffinityList           => undef,
     # value to override default deduplication_timeout_interval setting
     vdoAlbireoTimeout         => undef,
     # whether to check latency limits being exceeded
     vdoCheckLatencies         => undef,
     # value to override default max-discard-sectors setting
     vdoMaxDiscardSectors      => undef,
     # value to override default max-requests-active setting
     vdoMaxRequestsActive      => undef,
     # value to override default min_deduplication_timer_interval setting
     vdoMinAlbireoTimer        => undef,
     #
     vdoModuleVersion          => undef,
     # whether to preload the block map page cache at startup
     vdoWarmup                 => undef,
     # whether to log verbose stats at shutdown
     verboseShutdownStatistics => 1,
     # The directory to put temp files in
     workDir                   => undef,
    );

########################################################################
# Creates a C<Permabit::BlockDevice::VDO>.
#
# @param stack      The StorageStack which owns this device
# @param arguments  A hashref of properties
#
# @return a new C<Permabit::BlockDevice::VDO>
##
sub new {
  my ($invocant, $stack, $arguments) = assertNumArgs(3, @_);
  my $class = ref($invocant) || $invocant;
  # We are only used as a base class.
  assertNe(__PACKAGE__, $class);
  my $self = $class->SUPER::new($stack, $arguments);
  assertDefined($self->{binaryDir});
  assertEqualNumeric($DEFAULT_BLOCK_SIZE, $self->{blockSize});

  $self->{physicalSize} //= ($self->{storageDevice}->getSize()
                             - $self->getMetadataSize());

  if (!defined($self->getModuleSourceDir())) {
    $self->setModuleSourceDir($self->{binaryDir});
  }

  if ($self->{disableAlbireo}) {
    $self->{expectIndexer} = 0;
  }

  $self->getMachine()->disableConsoleBlanking();
  $self->installModule();
  return $self;
}

########################################################################
# @inherit
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2,  @_);
  $self->SUPER::configure($arguments);
  my $path = $self->{storageDevice}->getDevicePath();
  my $output = $self->runOnHost(["sudo wipefs --all --force $path"]);
  $log->info("$output");
  my $size = $self->{physicalSize};
  if (defined($size)) {
    $self->resizeStorageDevice($size);
  }

  $self->{moduleVersion} //= $self->{vdoModuleVersion};
  $self->{moduleVersion} //= $VDO_MARKETING_VERSION;

  if ($self->{useDistribution} || $self->{useUpstreamModule}) {
    $self->getMachine()->{userBinaryDir} = "/usr/bin";
  }
}

########################################################################
# @inherit
##
sub checkStorageDevice {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::checkStorageDevice();

  if (!$self->{stackable}) {
    # Make sure there are no VDOs below us.
    assertEqualNumeric(0, scalar($self->getAncestorsOfType(__PACKAGE__)),
                       "too many VDO instances");
  }
}

########################################################################
# @inherit
##
sub makeBackingDevice {
  my ($self) = assertNumArgs(1, @_);
  my $stack  = $self->{stack};
  $self->{storageDevice} = $stack->create('linear');
}

########################################################################
# Change a set of VDO properties on the VDO volume via lvchange.
# Property changes will take effect after the next VDO restart.
#
# @param args  a hashref of preperty-value pairs
##
sub changeVDOSettings {
  my ($self, $args) = assertNumArgs(2, @_);
  # This operation is only defined for LVMVDO devices right now.
  confess("Failed to override the changeVDOSettings method");
}

########################################################################
# @inherit
##
sub start {
  my ($self) = assertNumArgs(1, @_);
  my $machineName = $self->getMachineName();
  my $moduleName = $self->getModuleName();

  if (%{$self->{pendingSettings}}) {
    $self->changeVDOSettings($self->{pendingSettings});
    $self->{pendingSettings} = {};
  }

  if ($self->{_modules}{$machineName}) {
    $self->{_modules}{$machineName}{$moduleName}->reload();
  }
  if ($self->{expectRebuild}) {
    my $start = sub { $self->SUPER::start(); };
    $self->getMachine()->withKernelLogErrorCheckDisabled($start, "rebuild");
    return;
  }

  $self->SUPER::start();
}

########################################################################
# @inherit
##
sub stop {
  my ($self) = assertNumArgs(1, @_);

  # Check to see if we need to revert a non-standard device state
  if ($self->{_writableStorageEnabled}) {
    $self->disableWritableStorage();
  }
  if ($self->{_readableStorageEnabled}) {
    $self->disableReadableStorage();
  }

  $self->SUPER::stop();
}

########################################################################
# Fill the index with synthetic records for steady state testing.
#
# @oparam forceRebuild  If true, set up index to rebuild on next load
##
sub fillIndex {
  my ($self, $forceRebuild) = assertMinMaxArgs([0], 1, 2, @_);

  my $path = $self->getVDOStoragePath();
  my $cmd = $self->findBinary("vdoFillIndex");
  if ($forceRebuild) {
    $cmd .= " --force-rebuild";
  }

  my $fillIndex = sub {
    $self->enableWritableStorage();
    my $output = $self->runOnHost("sudo $cmd $path");
    $log->debug($output);
    $self->disableWritableStorage();
  };
  $self->runWhileStopped($fillIndex);
}

########################################################################
# @inherit
##
sub postActivate {
  my ($self) = assertNumArgs(1, @_);
  $self->addPreDeactivationStep(sub { $self->preDeactivateVDO(); }, 0);

  $self->{instance} = $self->getInstance();
  if (defined($self->{instance})) {
    $log->info("started $self->{vdoSymbolicPath} instance $self->{instance}");
  }

  if (!$self->{disableAlbireo} && $self->{albFill}) {
    $self->waitForIndex();
    $self->{albFill} = 0;
    $self->fillIndex();
  }

  # In testing, we can try out different queue priorities
  while (my ($k, $v) = each(%{$self->{queuePriorities}})) {
    $self->sendMessage("priority $k $v");
  }

  $self->SUPER::postActivate();

  if ($self->{vdoWarmup}) {
    # Initialize the block map so vdoWarmup does something.
    if (!defined($self->{_blockMapInitialized})) {
      $self->_doVDOInitializeBlockMap();
      $self->{_blockMapInitialized} = 1;
      # Restart to save the block map, which is currently only in memory. The
      # restart will finish starting, so return afterward.
      $self->restart();
      return;
    }
    $self->_doVDOWarmup();
  }

  my %kernelPidMap = $self->_getKernelThreadIDs();
  $log->debug("kernel pids: " . join(", ", sort(keys(%kernelPidMap))));

  # How should this interact (if at all) with the cpusAllowed property
  # supported in the FIO performance tests??
  if (defined($self->{vdoAffinity})) {
    if (defined($self->{vdoAffinityList})) {
      croak("vdoAffinity and vdoAffinityList cannot both be specified");
    }
    $self->setAffinity($self->{vdoAffinity});
  } elsif (defined($self->{vdoAffinityList})) {
    $self->setAffinityList($self->{vdoAffinityList});
  }

  # These parameters are only defined on non-release builds
  eval {
    foreach my $histogram (keys(%LATENCY_CHECKS)) {
      $self->sendMessage("histogram_limit $histogram " . secondsToMS($self->{latencyLimit}));
    }
  };

  if ($self->{expectIndexer}) {
    # Wait for the index to come online, so that tests can require a specific
    # amount of dedupe.
    $self->waitForIndex();
  }

  # If there was an error before starting this VDO instance, or an error in
  # starting this VDO instance.  We want to know about it before we start using
  # this VDO instance.
  $self->checkForKernelLogErrors();
  $self->{expectRebuild} = 0;
}

########################################################################
# Find the files necessary for full device operation.
##
sub resolveBinaries {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getMachine();
  $self->SUPER::resolveBinaries();
  if (!$self->{useDistribution}) {
    assertDefined($machine->findNamedExecutable("vdoaudit"));
  }
  assertDefined($machine->findNamedExecutable("vdoreadonly"));
}

########################################################################
# @inherit
##
sub saveLogFiles {
  my ($self, $saveDir) = assertNumArgs(2, @_);

  for my $host (keys(%{$self->{_modules}})) {
    my $hostDir = makeFullPath($saveDir, $host);
    mkpath($hostDir);

    copyRemoteFilesAsRoot($host, "/", "var/lib/dkms/", $hostDir);
  }

  $self->SUPER::saveLogFiles($saveDir);
}

########################################################################
# Prepare to deactivate.
##
sub preDeactivateVDO {
  my ($self) = assertNumArgs(1, @_);
  delayFailures(sub { $self->syncForStopStats(); },
                sub { $self->logStatsAtStop(); },
                sub { $self->checkForKernelLogErrors(); },
                sub { $self->assertPerformanceExpectations(); });
}

########################################################################
# A stop step to sync any outstanding writes before we log stats.
##
sub syncForStopStats {
  my ($self) = assertNumArgs(1, @_);
  $self->runOnHost("timeout -s KILL 1200 sync");
}

########################################################################
# A stop step to log the device statistics, doing a vdoSync first. This command
# will open and close "/dev/mapper/vdo0". A side effect of this is that the
# udevd system daemon will run "/sbin/blkid -o udev -p /dev/dm-0" or some such.
##
sub logStatsAtStop {
  my ($self) = assertNumArgs(1, @_);
  $self->getVDOStats()->logStats("$self->{vdoSymbolicPath} - before tear-down");
  if ($self->{verboseShutdownStatistics}) {
    $self->logHistograms();
  }
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);
  # Make sure to complete teardown even if we encounter errors.
  my @steps
    = (
       sub {
         $self->stop();
       },
       $self->getUninstallSteps(),
       sub {
         $self->SUPER::teardown();
       },
      );
  delayFailures(@steps);

  $self->checkForKernelLogErrors();
}

########################################################################
# Start the device with a force rebuild, assuming the device is already
# stopped.
##
sub forceRebuild {
  my ($self) = assertNumArgs(1, @_);
  confess("Failed to override the forceRebuild method");
}

########################################################################
# @inherit
##
sub recover {
  my ($self) = assertNumArgs(1, @_);
  $self->{expectRebuild} = 1;
  $self->SUPER::recover();
}

########################################################################
# Resume the device
##
sub resume {
  my ($self) = assertNumArgs(1, @_);
  $self->runOnHost("sudo dmsetup resume " . $self->getVDODeviceName());
}

########################################################################
# Suspend the device
##
sub suspend {
  my ($self) = assertNumArgs(1, @_);
  $self->runOnHost("sudo dmsetup suspend " . $self->getVDODeviceName());
}

########################################################################
# Get the configured module version.
##
sub getModuleVersion {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{useUpstreamModule}) {
    # Concurrent tests on multiple machines can collide accessing NFS
    # homedirs.
    my $scratchDir = $self->getMachine()->getScratchDir();
    my $xdgStateDir = "$scratchDir/xdgState";
    my $xdgCacheDir = "$scratchDir/xdgCache";
    my $getVerCmd = "XDG_STATE_HOME=$xdgStateDir " .
                    "XDG_CACHE_HOME=$xdgCacheDir " .
                    "yum list $VDO_USER_MODNAME.`uname -m` | " .
                    "awk '/^$VDO_USER_MODNAME/ {print \$2}'";
    my @ver = split(/\./, $self->runOnHost($getVerCmd));
    $self->setModuleVersion("$ver[0].$ver[1]");
  }
  return $self->{moduleVersion};
}

########################################################################
# Set the configured module version.
#
# @param  $versionString  the string representation of the module version
##
sub setModuleVersion {
  my ($self, $versionString) = assertNumArgs(2, @_);
  $self->{moduleVersion} = $versionString;
}

########################################################################
# @inherit
##
sub getModuleName {
  my ($self) = assertNumArgs(1, @_);

  # If useUpstreamModule is set, we need to use $VDO_UPSTREAM_MODNAME(dm-vdo)
  # module otherwise we load the $VDO_MODNAME(kvdo)
  if ($self->{useUpstreamModule}) {
    return $VDO_UPSTREAM_MODNAME;
  }
  return $self->SUPER::getModuleName();
}

########################################################################
# Set kernel module parameters
##
sub _setModuleParameters {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getMachine();
  my %MOD_PARAMS = (
                    vdoAlbireoTimeout    => "deduplication_timeout_interval",
                    vdoMaxRequestsActive => "max_requests_active",
                    vdoMinAlbireoTimer   => "min_deduplication_timer_interval",
                   );
  while (my ($prop, $param) = each(%MOD_PARAMS)) {
    if (defined($self->{$prop})) {
      my $path = $self->getSysModulePath($param);
      $machine->setProcFile($self->{$prop}, $path);
    }
  }
}

########################################################################
# Install the kernel and user tools modules
##
sub installModule {
  my ($self) = assertNumArgs(1, @_);
  my $machineName = $self->getMachineName();
  my $moduleName = $self->getModuleName();

  my $version = $self->getModuleVersion();

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
                                  machine         => $self->getMachine(),
                                  modDir          => $self->getModuleSourceDir(),
                                  modFileName     => "kmod-$moduleName",
                                  modName         => $moduleName,
                                  modVersion      => $version,
                                  useDistribution => $self->{useDistribution},
                                  useUpstream     => $self->{useUpstreamModule},
                                 );
  $module->load();
  $self->{_modules}{$machineName}{$moduleName} = $module;
  $self->_setModuleParameters();

  $log->info("Installing module $VDO_USER_MODNAME $version on $machineName");
  my $userModule
    = Permabit::UserModule->new(
                                machine         => $self->getMachine(),
                                modDir          => $self->getModuleSourceDir(),
                                modName         => $VDO_USER_MODNAME,
                                modVersion      => $version,
                                useDistribution => $self->{useDistribution},
                                useUpstream     => $self->{useUpstreamModule},
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
    # Always follow up mucking around with modules with a sync, in
    # case we force a crash or unclean reboot.
    $self->runOnHost("sync");
  }
}

########################################################################
# Uninstall the kernel and user tools modules
##
sub uninstallModule {
  my ($self, $machineName) = assertMinMaxArgs(1, 2, @_);
  $machineName //= $self->getMachineName();
  $log->info("Uninstalling modules from $machineName");
  if (defined($self->{_modules}{$machineName})) {
    # VDO-5320: Remove workaround symlink.
    if (defined($self->{_dmSymlink}{$machineName})) {
      $self->runOnHost("sudo rm -f $self->{_dmSymlink}{$machineName}");
      delete($self->{_dmSymlink}{$machineName});
    }
  }

  # Remove any installed modules
  foreach my $moduleName ($VDO_USER_MODNAME, $self->getModuleName()) {
    if (defined($self->{_modules}{$machineName}{$moduleName})) {
      $self->{_modules}{$machineName}{$moduleName}->unload();
      delete($self->{_modules}{$machineName}{$moduleName});
    }
  }

  # Memory leaks are not logged until the module is uninstalled
  $self->checkForKernelLogErrors();
}

########################################################################
# Uninstall the kernel module
##
sub getUninstallSteps {
  my ($self) = assertNumArgs(1, @_);
  my @steps = ();
  for my $machineName (keys(%{$self->{_modules}})) {
    push(@steps, sub { $self->uninstallModule($machineName); });
  }
  return @steps;
}

########################################################################
# Grow the logical size of the device.
#
# @param  logicalSize  The new logical size
##
sub growLogical {
  my ($self, $logicalSize) = assertNumArgs(2, @_);
  confess("Failed to override the growLogical method");
}

########################################################################
# Grow the physical size of the device.
#
# @param  physicalSize  The new physical size
##
sub growPhysical {
  my ($self, $physicalSize) = assertNumArgs(2, @_);
  confess("Failed to override the growPhysical method");
}

########################################################################
# Get the status of this device
#
# @oparam options  Optional additional options to pass to dmsetup status
#
# @return    the status string
##
sub getStatus {
  my ($self, $options) = assertMinMaxArgs([""], 1, 2, @_);
  my $output
    = $self->runOnHost("sudo dmsetup status $self->{vdoDeviceName} $options");
  $log->debug("status output: $output");
  return $output;
}

########################################################################
# Wait until the index service is available.
#
# @oparam statusList  Listref of acceptable status strings.  Defaults to
#                     "online" only.
# @oparam timeout     The maximum time to wait, in seconds. Defaults to a value
#                     that depends upon the type of the host.
##
sub waitForIndex {
  my %OPTIONS = (
                 statusList => [qw(online)],
                 timeout    => undef,
                );
  my ($self, $options) = assertOptionalArgs(1, \%OPTIONS, @_);
  my %statusMap = map { $_ => 1 } @{$options->{statusList}};
  my $timeout = $options->{timeout};
  $timeout //= $self->{expectRebuild} ? 30 * $MINUTE : undef;
  $timeout //= $self->supportsPerformanceMeasurement() ? 30 : 4 * $MINUTE;
  retryUntilTimeout(sub {
                      my $state = $self->getVDODedupeStatus();
                      if ($state) {
                        $log->debug("Index status is $state");
                        return $statusMap{$state};
                      }
                      return 0;
                    },
                    "Index service not ready", $timeout);
}

########################################################################
# Get the compression status of the VDO device.
#
# @return the contents of the sysfs compression entry for the device
##
sub getVDOCompressionStatus {
  my ($self) = assertNumArgs(1, @_);
  my $status = $self->getStatus();
  my @fields = split(' ', $status);
  return $fields[7];
}

########################################################################
# Get the dedupe status of the VDO device.
#
# @return the value of the dedupe state from dmsetup status call
##
sub getVDODedupeStatus {
  my ($self) = assertNumArgs(1, @_);
  my $status = $self->getStatus();
  my @fields = split(' ', $status);
  return $fields[6];
}

########################################################################
# Get the underlying storage device used.
#
# @return the storage device
##
sub getStorageDevice {
  my ($self) = assertNumArgs(1, @_);
  return $self->{storageDevice};
}

########################################################################
# Get the path of the storage device.
##
sub getStoragePath {
  my ($self) = assertNumArgs(1, @_);
  return $self->{storageDevice}->getDevicePath();
}

########################################################################
# Get the path of the vdo storage device.
##
sub getVDOStoragePath {
  my ($self) = assertNumArgs(1, @_);
  return $self->getStoragePath();
}

########################################################################
# Resize the physical size of the storage device.
#
# @param physicalSize   The new physical size, in bytes
##
sub resizeStorageDevice {
  my ($self, $physicalSize) = assertNumArgs(2, @_);
  my $newSize = $self->getTotalSize($physicalSize);
  $self->{storageDevice}->resize($newSize);
}

########################################################################
# Get the table entry for this device
#
# @oparam options  Optional additional options to pass to dmsetup table
#
# @return   the table entry string
##
sub getTable {
  my ($self, $options) = assertMinMaxArgs([""], 1, 2, @_);
  my $output
    = $self->runOnHost("sudo dmsetup table $self->{vdoDeviceName} $options");
  $log->debug("table output: $output");
  return $output;
}

########################################################################
# Disable the compression on a VDO device
##
sub disableCompression {
  my ($self)  = assertNumArgs(1, @_);
  my $state = $self->isVDOCompressionEnabled();
  if ($state) {
    $self->sendMessage("compression off");
  } else {
    $log->info("compression already disabled");
  }
}

########################################################################
# Disable the deduplication on a VDO device
##
sub disableDeduplication {
  my ($self)  = assertNumArgs(1, @_);
  my $state = $self->isVDODedupeEnabled();
  if ($state) {
    $self->sendMessage("index-close");
  } else {
    $log->info("deduplication already disabled");
  }
}

########################################################################
# Enable the compression on a VDO device
##
sub enableCompression {
  my ($self)  = assertNumArgs(1, @_);
  my $state = $self->isVDOCompressionEnabled();
  if (!$state) {
    $self->sendMessage("compression on");
  } else {
    $log->info("compression already enabled");
  }
}

########################################################################
# Enable the deduplication on a VDO device
##
sub enableDeduplication {
  my ($self)  = assertNumArgs(1, @_);
  my $state = $self->isVDODedupeEnabled();
  if (!$state) {
    $self->sendMessage("index-enable");
    $self->waitForIndex();
  } else {
    $log->info("deduplication already enabled");
  }
}

########################################################################
# Return whether compression is enabled.
#
# @return    1 = compressing, 0 = not compressing
##
sub isVDOCompressionEnabled {
  my ($self) = assertNumArgs(1, @_);
  return ($self->getVDOCompressionStatus() eq 'online');
}

########################################################################
# Return whether dedupe is enabled.
#
# @return    1 = deduplicating, 0 = not deduplicating
##
sub isVDODedupeEnabled {
  my ($self) = assertNumArgs(1, @_);
  return ($self->getVDODedupeStatus() eq 'online');
}

########################################################################
# Initialize the block map of the VDO.
##
sub _doVDOInitializeBlockMap {
  my ($self)  = assertNumArgs(1, @_);
  my $machine = $self->getMachine();
  assertDefined($self->{vdoSymbolicPath});

  my $vdoInitializeBlockMapPath
    = $machine->makeNfsSharePath($VDO_INITIALIZE_BLOCK_MAP);
  $self->runOnHost("sudo $vdoInitializeBlockMapPath $self->{vdoSymbolicPath}");
}

########################################################################
# Load block map data into the cache (as much as will fit).
##
sub _doVDOWarmup {
  my ($self) = assertNumArgs(1, @_);
  assertDefined($self->{vdoSymbolicPath});

  my $vdoWarmupPath = $self->getMachine()->makeNfsSharePath($VDO_WARMUP);
  $self->runOnHost("sudo $vdoWarmupPath $self->{vdoSymbolicPath}");
}

########################################################################
# Wait for all operations, including deduplication, to complete.
##
sub doVDOSync {
  my ($self) = assertNumArgs(1, @_);
  # Is the VDO suspended? If so we can't fsync, but stats are already
  # stable.
  my $dmInfo = $self->runOnHost("sudo dmsetup info $self->{vdoDeviceName}");
  if ($dmInfo =~ m/SUSPENDED/) {
    return;
  }
  assertDefined($self->getDevicePath());
  # In read only mode, this will fail, but in read only mode write stats
  # are stable anyway.
  eval {
    $self->getMachine()->fsync($self->getDevicePath());
  };
  my $error = $EVAL_ERROR;
  if ($error && ($error !~ m/Input\/output error/)) {
    croak($error);
  }
}

########################################################################
# Brings down the readable vdo storage device.
##
sub disableReadableStorage {
  my ($self) = assertNumArgs(1, @_);
  $self->{_readableStorageEnabled} = 0;
}

########################################################################
# Brings up the vdo storage as a readable device.
##
sub enableReadableStorage {
  my ($self) = assertNumArgs(1, @_);
  $self->{_readableStorageEnabled} = 1;
}

########################################################################
# Brings down the writable vdo storage device.
##
sub disableWritableStorage {
  my ($self) = assertNumArgs(1, @_);
  $self->{_writableStorageEnabled} = 0;
}

########################################################################
# Brings up the vdo storage as a writable device.
##
sub enableWritableStorage {
  my ($self) = assertNumArgs(1, @_);
  $self->{_writableStorageEnabled} = 1;
}

########################################################################
# Audit the VDO.
##
sub doVDOAudit {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{converted}) {
    $log->info("Not auditing the VDO because it has been converted.");
    return {returnValue => 0};
  }

  my $path = $self->getVDOStoragePath();
  my $audit = $self->getMachine()->findNamedExecutable("vdoaudit");

  $self->enableReadableStorage();
  my $output = runCommand($self->getMachine()->getName(),
                          "sudo $audit --verbose $path");
  $self->disableReadableStorage();

  return $output;
}

########################################################################
# Dump the VDO metadata.
#
# @oparam suffix  The suffix for the metadata path name.  Defaults to "dump".
##
sub dumpMetadata {
  my ($self, $suffix) = assertMinMaxArgs(["dump"], 1, 2, @_);
  if ($self->{converted}) {
    $log->info("Not dumping metadata because the VDO has been converted.");
    return {returnValue => 0};
  }

  my $path = $self->getVDOStoragePath();
  my $dumpMetadata = $self->getMachine()->findNamedExecutable("vdodumpmetadata");
  my $dump = "$self->{runDir}/$self->{vdoDeviceName}-$suffix";
  my $noBlockMap = "";
  if (defined($self->{logicalSize}) && ($self->{logicalSize} > 8 * $TB)) {
    # Don't save the block map for VDOs with a lot of logical space
    # (8 TB logical = 10 GB of block map)
    $noBlockMap = "--no-block-map";
  }

  $self->enableReadableStorage();
  my $output = $self->runOnHost(["sudo $dumpMetadata $path $noBlockMap $dump",
                                 "sudo gzip -f $dump"]);
  $self->disableReadableStorage();

  return $output;
}

########################################################################
# Corrupt the VDO storage.
##
sub corruptPBNRef {
  my ($self) = assertNumArgs(1, @_);
  my $path = $self->getVDOStoragePath();
  my $corrupt = $self->findBinary("corruptPBNRef");

  $self->enableWritableStorage();
  my $output = runCommand($self->getMachine()->getName(),
                          "sudo $corrupt $path");
  $self->disableWritableStorage();

  return $output;
}

########################################################################
# Force the VDO into read-only mode.
##
sub setReadOnlyMode {
  my ($self)     = assertNumArgs(1, @_);
  my $path       = $self->getVDOStoragePath();
  my $readonly   = $self->getMachine()->findNamedExecutable("vdoreadonly");
  my $output;

  my $setReadOnly = sub {
    $self->enableWritableStorage();
    $output = $self->runOnHost("sudo $readonly $path");
    $self->{expectIndexer} = 0;
    $self->disableWritableStorage();
  };
  $self->runWhileStopped($setReadOnly);

  return $output;
}

########################################################################
# Force a full rebuild when VDO next starts by running vdoForceRebuild
# on the VDO backing store.
##
sub runVDOForceRebuild {
  my ($self) = assertNumArgs(1, @_);
  my $path = $self->getVDOStoragePath();
  my $force = $self->getMachine()->findNamedExecutable("vdoforcerebuild");

  $self->enableWritableStorage();
  my $output = runCommand($self->getMachine()->getName(),
                          "sudo $force $path");
  $self->disableWritableStorage();

  return $output;
}

########################################################################
# Hook/factory method to construct the c vdostats command string.
#
# @oparam args  the arguments to the command
#
# @return a new C<Permabit::CommandString::VDOStats>
##
sub makeVDOStatsCommandString {
  my ($self, $args) = assertNumArgs(2, @_);
  my $machine = $self->getMachine();
  my $params = {
                binary  => $machine->findNamedExecutable("vdostats"),
                %$args,
               };
  return Permabit::CommandString::VDOStats->new($self, $params);
}

########################################################################
# Generate the command to run vdoStats with the specified options
#
# @oparam args        arguments to the command
# @oparam allVolumes  If true, will gather statistics for all volumes,
#                     otherwise just for the one associated with this device
#
# @return the raw output of the vdoStats command
##
sub runVDOStatsCommand {
  my ($self, $args, $allVolumes) = assertMinMaxArgs([{}, 0], 1, 3, @_);
  my $machine = $self->getMachine();

  my %arguments = ( doSudo => 1, %{$args}, );
  $arguments{deviceName} = $allVolumes ? undef : $self->{vdoSymbolicPath};

  my $command = $self->makeVDOStatsCommandString(\%arguments);
  $machine->executeCommand("$command");
  return $machine->getStdout();
}

########################################################################
# Read the current VDO statistics.  Do not wait for VDO to finish any
# operations in progress.
#
# @oparam class  The statistics class to instantiate, defaults to
#                C<Permabit::Statistics::VDO>.
#
# @return the VDO statistics
##
sub getCurrentVDOStats {
  my ($self, $class) = assertMinMaxArgs(['Permabit::Statistics::VDO'],
                                        1, 2, @_);

  my $rawStats = $self->runVDOStatsCommand({ all => 1 });
  my $stats = yamlStringToHash($rawStats)->{$self->{vdoSymbolicPath}};
  assertDefined($stats, "stats must be tagged '$self->{vdoSymbolicPath}'");

  eval("use $class;");
  if ($EVAL_ERROR) {
    croak("Could not use $class : $EVAL_ERROR");
  }
  return $class->new(%{$stats});
}

########################################################################
# Wait for dedupe operations to finish and then read the current stats.
#
# @return The stats or undef if not running
##
sub getVDOStats {
  my ($self) = assertNumArgs(1, @_);
  $self->doVDOSync();
  return $self->getCurrentVDOStats();
}

########################################################################
# Read the current human-relatable VDO statistics.
#
# @param args         arguments to the command; expected to contain the
#                     human-readable option to use
# @oparam allVolumes  If true, will gather statistics for all volumes,
#                     otherwise just for the one associated with this device
#
# @return the human-relatable VDO statistics (text)
##
sub getHumanVDOStats {
  my ($self, $args, $allVolumes) = assertMinMaxArgs([0], 2, 3, @_);
  $self->doVDOSync();
  return $self->runVDOStatsCommand($args, $allVolumes);
}

########################################################################
# Log the human-relatable VDO statistics.
#
# @param args   arguments to the command; expected to contain the
#               human-readable option to use
##
sub logHumanVDOStats {
  my ($self, $args) = assertNumArgs(2, @_);
  $log->info("Human-readable stats: " . $self->getHumanVDOStats($args));
}

########################################################################
# Compare two histogram buckets for sorting.  The "Bigger" bucket is
# always last.
##
sub compare_ranges {
  if ($a eq "Bigger") {
    return 1;
  }
  if ($b eq "Bigger") {
    return -1;
  }
  my ($lowera) = $a =~ m/^(\d+)/;
  my ($lowerb) = $b =~ m/^(\d+)/;
  return $lowera <=> $lowerb;
}

########################################################################
# Show histogram detailed output, including bars to help visualize the
# distribution of values.
#
# @param histogram   The histogram to output
#
##
sub logHistogramBars {
  my ($self, $histogram) = assertNumArgs(2, @_);

  if ($histogram->{"unit"}) {
    $log->debug($histogram->{"label"} . " Histogram - number of "
                . $histogram->{"types"} . " by "
                . $histogram->{"metric"} . " ("
                . $histogram->{"unit"} . ")");
  } else {
    $log->debug($histogram->{"label"} . " Histogram - number of "
                . $histogram->{"types"} . " by "
                . $histogram->{"metric"});
  }

  my $buckets = $histogram->{"buckets"};

  my $total = 0;
  foreach my $bucketRange ((keys %{ $buckets })) {
    $total = $total + $buckets->{$bucketRange};
  }

  foreach my $bucketRange (sort compare_ranges (keys %{ $buckets })) {
    my $value = $buckets->{$bucketRange};

    my $barLength;
    if ($total > 0) {
      $barLength = int(($value * 50) / $total) + 1;
      if ($barLength == 1) {
        $barLength = 0;
      }
    } else {
      $barLength = 0;
    }

    my $rangeString;
    my $barString;
    if ($histogram->{"logarithmic"}) {
      if ($bucketRange eq "Bigger") {
        $rangeString = sprintf("%-16s", $bucketRange);
      } else {
        my ($lower, $upper) = $bucketRange =~ m/^(\d+) - (\d+)/;
        $rangeString = sprintf("%6d - %7d", $lower, $upper);
      }
    } else {
      if ($bucketRange eq "Bigger") {
        $rangeString = sprintf("%6s", $bucketRange);
      } else {
        $rangeString = sprintf("%6d", int($bucketRange));
      }
    }

    $barString = sprintf(" : %12llu%s\n", $value, '=' x $barLength);
    $log->debug("$rangeString $barString");
  }
  $log->debug("total $total");
}

########################################################################
# Log the expected VDO histograms.
#
# @oparam histograms  Names of the histograms to be logged. If the list is
#                     empty, all of the histograms are logged. Names
#                     may be one pathname component ("read_queue"), or
#                     multiple ("physQ1/wakeup_latency") for work queue stats.
##
sub logHistograms {
  my ($self, @histograms) = assertMinArgs(1, @_);
  my $machine = $self->getMachine();
  # Histograms are subject to the VDO_INTERNAL build macro.  Release builds
  # will not have histograms.
  my $output;
  eval {
    $self->sendMessage("histograms");
    $output = $machine->getStdout();
  };
  if ($EVAL_ERROR) {
    return;
  }
  my $histogramSet = decode_json($output);

  $log->info("Logging histograms for $self->{vdoSymbolicPath}");
  foreach my $histogram (@$histogramSet) {
    if ((scalar(@histograms) > 0) && (!grep { $histogram->{"name"} =~ m/$_/ } @histograms)) {
      next;
    }
    my $label = $histogram->{"label"};
    my $mean = $histogram->{"mean"};
    # If the mean is undefined, it means that there were no histogram samples.
    if (defined($mean)) {
      my $maximum = $histogram->{"maximum"};
      my $minimum = $histogram->{"minimum"};
      my $unit = $histogram->{"unit"};

      $self->logHistogramBars($histogram);

      if (defined($unit)) {
        $log->debug("  $label unit is $unit");
      } else {
        $log->debug("  $label is unitless");
      }
      my %timeScales = (
                        "milliseconds" => 1.0e-3,
                        "microseconds" => 1.0e-6,
                       );
      if (defined($unit) && defined($timeScales{$unit})) {
        for my $variable (\$mean, \$minimum, \$maximum) {
          if (defined($$variable)) {
            $$variable = timeToText($$variable * $timeScales{$unit});
          }
        }
      }
      $log->debug("  $label mean is $mean");
      if (defined($minimum)) {
        $log->debug("  $label minimum is $minimum");
      }
      if (defined($maximum)) {
        $log->debug("  $label maximum is $maximum");
      }
    }
  }
}

########################################################################
# Get a statistic from a histogram
#
# @param name  The histogram name
# @param type  The statistic type ("mean" or "maximum" or "unacceptable")
#
# @return the statistic.  If the statistic is not available, the value undef is
#         returned.  The values of the statistic type "mean" or "maximum" are
#         time measurements in milliseconds, which we convert to seconds.  The
#         values of the statistic "unacceptable" are a count;
##
sub _getHistogramStatistic {
  my ($self, $name, $type) = assertNumArgs(3, @_);
  my $value = $self->_getHistogramString($name, $type);
  if (!defined($value) || ($value eq "0/0")) {
    # The value "0/0" means that there were no samples.
    return undef;
  }
  # Convert to a number
  return 0 + $value;
}

########################################################################
# Get the units used for a histogram, or undef if none
#
# @param name   The histogram name
#
# @return the unit name, if any, or undef
##
sub _getHistogramUnits {
  my ($self, $name) = assertNumArgs(2, @_);
  my $value = $self->_getHistogramString($name, "unit");
  if (!defined($value) || ($value eq "")) {
    # An empty result means no unit is defined.
    return undef;
  }
  return $value;
}

########################################################################
# Get a text string from a histogram sysfs tree
#
# @param name   The histogram name
# @param label  The label of the thing to read
#
# @return the string, if available, or undef
##
sub _getHistogramString {
  my ($self, $name, $label) = assertNumArgs(3, @_);
  my $path = $self->getSysModuleDevicePath("$name/$label");
  my $value = eval { return $self->getMachine()->catAndChomp($path); };
  if ($EVAL_ERROR) {
    # Histograms are subject to the VDO_INTERNAL build macro.  Release builds
    # will not have histograms.
    return undef;
  }
  return $value;
}

########################################################################
# Verify that the basic and universal performance expectations have been met.
##
sub assertPerformanceExpectations {
  my ($self) = assertNumArgs(1, @_);

  if (!$self->{started}) {
    return;
  }

  my $checkLatencies
    = $self->{vdoCheckLatencies} // $self->supportsPerformanceMeasurement();
  if (!$checkLatencies) {
    return;
  }

  # Okay, we actually do want the checks this time.

  # Assert that the maximum time to acknowledge an I/O request does not
  # exceed 30 seconds.  This time is chosen to be less than (but near) 60
  # seconds, which is the timeout used by the SCST driver software.  This
  # software is used to export block devices (using fibre channel or ISCSI),
  # and it will timeout any I/O that takes more than 60 seconds.  We also
  # check the storage device under VDO.
  my @checks;
  foreach my $histogram (sort(keys(%LATENCY_CHECKS))) {
    my $value = $self->_getHistogramStatistic($histogram, "unacceptable");
    my $message = "$LATENCY_CHECKS{$histogram} latency is too high";
    if (defined($value)) {
      push(@checks, sub { assertEqualNumeric(0, $value, $message); });
    }
  }
  delayFailures(@checks);
}

########################################################################
# Verify that the deduplication service is online.
##
sub assertDeduplicationOnline {
  my ($self) = assertNumArgs(1, @_);
  assertEq("online", $self->getVDODedupeStatus());
}

########################################################################
# Verify that the deduplication service is offline.
##
sub assertDeduplicationOffline {
  my ($self) = assertNumArgs(1, @_);
  assertEq("offline", $self->getVDODedupeStatus());
}

########################################################################
# @inherit
##
sub check {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::check();
  $self->assertPerformanceExpectations();
}

########################################################################
# Send a dmsetup message to the device
#
# @param message  The message string to send
##
sub sendMessage {
  my ($self, $message) = assertNumArgs(2, @_);
  $self->runOnHost("sudo dmsetup message $self->{vdoDeviceName} 0 $message");
}

########################################################################
# Get the device pathname of the VDO device
#
# @return the device pathname
##
sub getVDODevicePath {
  my ($self) = assertNumArgs(1, @_);
  assertDefined($self->{vdoSymbolicPath});

  # Resolve the device path name if it's a symlink because tools
  # that need to reference things in sysfs need to know the actual
  # device name. It's important to resolve the sym link every time
  # because on reboots, the device paths can change.
  return $self->_resolveSymlink($self->{vdoSymbolicPath});
}

########################################################################
# Get the major/minor device numbers for a VDO device.
#
# @return the major and minor device numbers
##
sub getVDODeviceMajorMinor {
  my ($self) = assertNumArgs(1, @_);
  my $errno  = $self->sendCommand('ls -Hl ' . $self->getVDODevicePath());
  assertEqualNumeric(0, $errno);
  my @majorMinor = ($self->getMachine()->getStdout()
                    =~ m/^b[rwx-]+T?\s+\d+[\s\w]+\s+(\d+),\s+(\d+)/);
  assertEqualNumeric(2, scalar(@majorMinor));
  return @majorMinor;
}

########################################################################
# Get the resolved name of the VDO device.
#
# @return the resolved device name
##
sub getVDODeviceResolvedName {
  my ($self) = assertNumArgs(1, @_);
  return basename($self->getVDODevicePath());
}

########################################################################
# Get the path to a sysfs file in the /sys/modules/kvdo/parameters tree.
#
# @param name  Relative path name to the sysfs file
#
# @return the absolute path name to the sysfs file
##
sub getSysModulePath {
  my ($self, $name) = assertNumArgs(2, @_);
  return makeFullPath("/sys/module", $self->getModuleName(), "parameters",
                      $name);
}

########################################################################
# Get the maximum discard sectors
#
# @return the maximum discard sectors
##
sub getMaxDiscardSectors {
  my ($self) = assertNumArgs(1, @_);
  my $path = $self->getSysModulePath("max_discard_sectors");
  return (eval { return $self->getMachine()->catAndChomp($path); }
          // $self->{blockSize} / $SECTOR_SIZE);
}

########################################################################
##
sub setAlbireoTimeout {
  my ($self, $timeout) = assertNumArgs(2, @_);
  my $path = $self->getSysModulePath("deduplication_timeout_interval");
  $self->getMachine()->setProcFile($timeout, $path);
}

########################################################################
# Wait until the VDO exits recovery mode.
#
# @oparam timeout   the timeout to use, in seconds
##
sub waitUntilRecoveryComplete {
  my ($self, $timeout) = assertMinMaxArgs([15 * $MINUTE], 1, 2, @_);
  my $condition = sub {
    return ($self->getCurrentVDOStats()->{"operating mode"} ne "recovering");
  };
  retryUntilTimeout($condition, "recovery did not complete", $timeout, 5);
  assertEq("normal", $self->getCurrentVDOStats()->{"operating mode"});
}

########################################################################
# Determine the kernel thread IDs and names used by all VDO devices.
#
# Threads associated with a particular VDO device might be useful
# someday, but that information is harder to extract.
#
# @return   a map of pid => name
##
sub _getKernelThreadIDs {
  my ($self) = assertNumArgs(1, @_);
  my $moduleName = $self->getModuleName();
  my $instance = $self->{instance} // "[0-9]+";
  # N.B.: This will fail if we get to three-digit instances, because
  # "kvdo100:journalQ" is too long for a process name and will be truncated.
  my $cmd = ("ps ax | "
             . "awk '/\\[${moduleName}${instance}:[a-zA-Z0-9]+Q[0-9]*\\]/"
             . " { print \$1, \$NF }'");
  return split(' ', $self->runOnHost($cmd));
}

########################################################################
# Parse a property or command-line argument giving CPU affinity specs.
#
# The argument can be in two general forms, using list or a
# hexadecimal mask; this comes from the taskset program. The list form
# generally looks like "1,3,5-9"; because runtests.pl likes to split
# arguments at commas into arrays, we'll accept an array form and
# rebuild the string before parsing. The infoType argument indicates
# whether the list or mask form is used.
#
# We accept an extended form that breaks down the affinity by worker
# thread type for kernel threads, using a prefix on a mask or CPU
# list: "AFFINITY,bio@AFFINITY,cpu@AFFINITY", where AFFINITY has
# either of the forms described above, though all of them must follow
# the same form. The first portion (without the TYPE@ prefix) is
# required.
#
# @param cpuInfo   The CPU list/mask argument(s)
#
# @return  a list (default-cpus, hashref { thread-type => cpus })
##
sub _parseAffinity {
  my ($cpuInfo) = assertNumArgs(1, @_);

  if (ref($cpuInfo) eq 'ARRAY') {
    $cpuInfo = join(',', @{$cpuInfo});
  }
  my %affinitiesByType;
  if ($cpuInfo =~ m/@/) {
    # Pick apart "AFF1,foo@AFF2,bar@AFF3" into sections, where each
    # AFF part can contain commas.
    my @segments = split(',', $cpuInfo);
    my $type = "";
    # Build arrays of strings with sublists of CPU numbers.
    foreach my $segment (@segments) {
      if ($segment =~ m/(.*)@(.*)/) {
        $type = $1;
        $segment = $2;
      }
      push(@{$affinitiesByType{$type}}, $segment);
    }
    # Change arrays of strings to joined strings.
    foreach my $type (keys(%affinitiesByType)) {
      $affinitiesByType{$type} = join(",", @{$affinitiesByType{$type}});
    }
    # Set cpuInfo to just the default.
    $cpuInfo = $affinitiesByType{""};
    delete $affinitiesByType{""};
  }
  return ($cpuInfo, \%affinitiesByType);
}

########################################################################
# Set the CPU affinity for VDO processes on the target machine.
#
# The format of the cpuInfo argument is as described above for
# _parseAffinity. The first portion of the cpuInfo argument (without
# the TYPE@ prefix) applies to the Albireo indexer threads.
#
# We don't support having different affinity settings for different
# VDO devices on one machine.
#
# @param  cpuInfo   a string or arrayref of strings describing the
#                   allowed CPUs
# @param  infoType  "c" for a comma-separated list of CPUs, "" for a mask
##
sub _setAffinity {
  my ($self, $cpuInfo, $infoType) = assertNumArgs(3, @_);
  my $requestedAffinities;
  ($cpuInfo, $requestedAffinities) = _parseAffinity($cpuInfo);

  my %kernelPids = $self->_getKernelThreadIDs();
  # Map from thread type (a simplified form of the thread name, e.g.,
  # "vdo1:BioQ3" becomes "bio") to arrayref of PIDs.
  my %threadsByType = ();
  foreach my $thread (keys(%kernelPids)) {
    my $type = $kernelPids{$thread};
    $type =~ s/^\[vdo\d+:(.*)Q\d*\]$/$1/;
    $type = lc($type);
    $threadsByType{$type} ||= ();
    push(@{$threadsByType{$type}}, $thread);
  }

  my @types = keys(%threadsByType);
  $log->debug("kernel thread types found: " . join(", ", sort(@types)));

  # Map from thread type to the desired affinity. If a thread type
  # wasn't specified via the test property, we use the fallback
  # affinity; given "1-3,bio@5-7", we'll map "cpu" to "1-3".
  my %affinitiesByThreadType =
    map { $_ => ($requestedAffinities->{$_} // $cpuInfo) } @types;
  # Just note for the record if anything was specified via the
  # property when no such threads were actually found.
  map { delete $requestedAffinities->{$_}; } @types;
  if (%{$requestedAffinities}) {
    $log->info("can't set affinity: no threads found for: "
               . join(",", sort(keys(%{$requestedAffinities}))));
  }

  # Regroup the process IDs by desired CPU affinity setting; we'll
  # map, for example, "5-7" to an arrayref of PIDs.
  my %threadsByAffinities;
  foreach my $type (keys(%affinitiesByThreadType)) {
    my $affinity = $affinitiesByThreadType{$type};
    $threadsByAffinities{$affinity} ||= ();
    push(@{$threadsByAffinities{$affinity}}, @{$threadsByType{$type}});
  }
  # For each distinct CPU affinity list specified, iterate over all
  # the PIDs to which it should apply.
  my @taskSetCommands =
    map {
      my @pids = @{$threadsByAffinities{$_}};
      my $pidList = join(' ', sort(@pids));
      "for pid in $pidList; do sudo taskset -p${infoType} $_ \$pid; done";
    } keys(%threadsByAffinities);
  my $output = $self->runOnHost(join('; ', '(set -e', @taskSetCommands) . ')');
  $log->debug($output);
}

########################################################################
# Set the CPU affinity for VDO processes on the target machine.
#
# @param  cpumask  a string holding a bit mask of allowed CPUs
##
sub setAffinity {
  my ($self, $cpumask) = assertNumArgs(2, @_);
  $self->_setAffinity($cpumask, "");
}

########################################################################
# Set the CPU affinity for VDO processes on the target machine.
#
# @param  cpulist  a string holding a comma-separated list of allowed CPUs
##
sub setAffinityList {
  my ($self, $cpulist) = assertNumArgs(2, @_);
  $self->_setAffinity($cpulist, "c");
}

########################################################################
# Fetch the instance number for the current device, if any.
#
# @return  the instance number or undef
##
sub getInstance {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getMachine();
  # may not exist for upgrade tests using older versions of VDO
  return eval {
    return $self->getVDOStats()->{"instance"};
  };
}

########################################################################
# Determine how much space the UDS index requires on the storage device.
##
sub calculateIndexStorageSize {
  my ($self) = assertNumArgs(1, @_);

  my $cmd = $self->findBinary("udsCalculateSize");
  if (defined($self->{memorySize})) {
    $cmd .= " --uds-memory-size=$self->{memorySize}";
  }

  if ($self->{sparse}) {
    $cmd .= " --uds-sparse";
  }

  my $output = $self->runOnHost($cmd);
  return ((0 + $output) * $DEFAULT_BLOCK_SIZE);
}

########################################################################
# Gets the device's extra logical metadata size. The default is 0.
#
# @return the extra logical metadata size
##
sub getLogicalMetadataSize {
  my ($self) = assertNumArgs(1, @_);
  return 0;
}

########################################################################
# Gets the device's extra physical metadata size.
#
# @return the extra metadata size
##
sub getMetadataSize {
  my ($self) = assertNumArgs(1, @_);
  $self->{metadataSize} //=
    ($DEFAULT_BLOCK_SIZE * 2) + $self->calculateIndexStorageSize();
  return $self->{metadataSize};
}

########################################################################
# Gets the amount of storage space used by a VDO device.
#
# @param physicalSize  The physical size of the VDO data region, in bytes
#
# @return the total number of bytes VDO will require on its storage
##
sub getTotalSize {
  my ($self, $physicalSize) = assertNumArgs(2, @_);
  return $physicalSize + $self->getMetadataSize();
}

########################################################################
# Gets the device name of the VDO volume.
#
# @return the device name of the VDO volume.
##
sub getVDODeviceName {
  my ($self) = assertNumArgs(1, @_);
  return $self->{vdoDeviceName};
}

########################################################################
# Gets the path to the VDO volume.
#
# @return the path to the VDO volume.
##
sub getVDOSymbolicPath {
  my ($self) = assertNumArgs(1, @_);
  return $self->{vdoSymbolicPath};
}

########################################################################
# @inherit
##
sub setDeviceName {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  $self->SUPER::setDeviceName($deviceName);
  $self->{vdoDeviceName} = $self->{deviceName};
}

########################################################################
# @inherit
##
sub setSymbolicPath {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::setSymbolicPath();
  $self->{vdoSymbolicPath} = $self->{symbolicPath};
}

1;
