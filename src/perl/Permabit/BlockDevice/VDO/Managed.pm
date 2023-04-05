##
# Perl object that represents a kernel VDO device managed by the vdo
# manager script
#
# $Id$
##
package Permabit::BlockDevice::VDO::Managed;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);
use File::Basename;
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertFalse
  assertMinArgs
  assertMinMaxArgs
  assertNumArgs
  assertRegexpMatches
  assertType
);
use Permabit::CommandString::VDO;
use Permabit::Constants;
use Permabit::ProcessUtils qw(delayFailures);
use Permabit::Utils qw(
  makeFullPath
  retryUntilTimeout
  sizeToLvmText
  yamlStringToHash
);
use YAML;

use base qw(Permabit::BlockDevice::VDO);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my @CLEANUP_FILES =  ("/etc/vdoconf.yml",
                      "/var/lock/vdo",
                      "/var/lock/vdo-config-singletons");

########################################################################
# @paramList{new}
#
our %BLOCKDEVICE_PROPERTIES
  = (
     # The path to the binaries (used by Permabit::CommandString::VDO)
     albireoBinaryPath => undef,
     # The configuration file
     confFile          => undef,
     # @ple the name of the (optional) manager log file
     managerLogFile    => "manager.log",
    );
##

########################################################################
# Creates a C<Permabit::BlockDevice::VDO::Managed>.
#
# @param stack      The StorageStack which owns this device
# @param arguments  A hashref of properties
#
# @return a new C<Permabit::BlockDevice::VDO::Managed>
##
sub new {
  my ($invocant, $stack, $arguments) = assertNumArgs(3, @_);
  my $class = ref($invocant) || $invocant;
  my $self = $class->SUPER::new($stack, $arguments);

  $self->registerFileCleanup();
  return $self;
}

########################################################################
# Register a cleanup step to deal with VDO files left by the manager.
##
sub registerFileCleanup {
  my ($self) = assertNumArgs(1, @_);
  my $cleanup = sub {
    my ($machine) = assertNumArgs(1, @_);
    my @files = (@CLEANUP_FILES, $self->{confFile});
    $machine->runSystemCmd(join(' ', 'sudo rm -rf', @files));
  };
  $self->getMachine()->addCleanupStep($cleanup);
}

########################################################################
# @inherit
##
sub installModule {
  my ($self) = assertNumArgs(1, @_);

  $self->{albireoBinaryPath} //= $self->{binaryDir};
  $self->{confFile} //= makeFullPath($self->{scratchDir}, "vdoconf.yml");

  if (defined($self->{managerLogFile}) && ($self->{managerLogFile} !~ m(/))) {
    $self->{managerLogFile}
      = makeFullPath($self->{runDir}, $self->{managerLogFile});
  }

  $self->SUPER::installModule();
}

########################################################################
# Create a hash of options based on the device properties, to pass to a
# VDO create or import command.
#
# @oparam moreOptions additional options to pass to create
#
# @return a hashref of device options
##
sub _prepareOptions {
  my ($self, %moreOptions) = assertMinArgs(1, @_);
  my $options = {
		 force    => 1,
		 indexMem => $self->{memorySize},
                 %moreOptions,
                };

  # Most options are simply copied through with new names.
  my %propertyNameMap = (
                         # createOption         => propertyName
                         vdoAckThreads          => 'bioAckThreadCount',
                         vdoBioRotationInterval => 'bioThreadRotationInterval',
                         vdoBioThreads          => 'bioThreadCount',
                         vdoCpuThreads          => 'cpuThreadCount',
                         vdoHashZoneThreads     => 'hashZoneThreadCount',
                         vdoLogicalThreads      => 'logicalThreadCount',
                         vdoPhysicalThreads     => 'physicalThreadCount',
                        );
  foreach my $createOption (keys(%propertyNameMap)) {
    my $propertyName = $propertyNameMap{$createOption};
    $options->{$createOption} = $self->{$propertyName};
  }

  # Options that aren't simply copied...
  if (defined($self->{blockMapCacheSize})) {
    $options->{blockMapCacheSize} = sizeToLvmText($self->{blockMapCacheSize});
  }
  if (defined($self->{sparse})) {
    $options->{sparseIndex}
      = $self->{sparse} ? "enabled" : "disabled";
  }
  if (defined($self->{emulate512Enabled})) {
    $options->{emulate512}
      = $self->{emulate512Enabled} ? "enabled" : "disabled";
  }
  if (defined($self->{logicalSize})) {
    $options->{vdoLogicalSize} = sizeToLvmText($self->{logicalSize});
  }
  if (defined($self->{enableCompression})) {
    # magic value -1 suppresses the option completely
    if ($self->{enableCompression} != -1) {
      $options->{compression}
        = $self->{enableCompression} ? "enabled" : "disabled";
    }
  }
  if (defined($self->{enableDeduplication})) {
    # magic value -1 suppresses the option completely
    if ($self->{enableDeduplication} != -1) {
      $options->{deduplication}
        = $self->{enableDeduplication} ? "enabled" : "disabled";
    }
  }
  if (defined($self->{vdoMaxDiscardSectors})) {
    $options->{maxDiscardSize}
      = ($self->{vdoMaxDiscardSectors} * 512) . "B";
  }
  if (defined($self->{slabBits})) {
    # 256 is 4K / 1M
    $options->{vdoSlabSize} = (2 ** $self->{slabBits} / 256) . "M";
  }
  return $options;
}

########################################################################
# Invoke the VDO manager script to create the device, using configuration
# parameters already defined in the object.
#
# @oparam moreOptions additional options to pass to create
##
sub doCreate {
  my ($self, %moreOptions) = assertMinArgs(1, @_);
  $log->info("Run vdo manager create for '$self->{deviceName}'");
  $self->assertVDOCommand("create", $self->_prepareOptions(%moreOptions));
}

########################################################################
# Invoke the VDO manager script to import the device, using configuration
# parameters already defined in the object.
#
# @oparam moreOptions additional options to pass to create
##
sub doImport {
  my ($self, %moreOptions) = assertMinArgs(1, @_);
  $log->info("Run vdo manager import for '$self->{deviceName}'");
  $self->assertVDOCommand("import", $self->_prepareOptions(%moreOptions));
}

########################################################################
# Invoke the VDO manager script to remove the device.
#
# @oparam okToFail    do or do not allow failure
# @oparam moreOptions options to pass to remove
##
sub doRemove {
  my ($self, $okToFail, %moreOptions) = assertMinArgs([1], 1, @_);
  if ($okToFail) {
    $self->runVDOCommand("remove", { %moreOptions });
  } else {
    $self->assertVDOCommand("remove", { %moreOptions });
  }
}

########################################################################
# @inherit
##
sub setup {
  my ($self) = assertNumArgs(1, @_);

  $self->doCreate(activate => 'disabled');

  # Activate.
  $self->assertVDOCommand("activate");

  if ($self->{disableAlbireo}) {
    $self->disableDeduplication();
  }

  # Always call SUPER::setup at end to do final initialization
  $self->SUPER::setup();
}

########################################################################
# @inherit
##
sub start {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{started}) {
    $log->info("not starting managed VDO since it should already be running");
    return;
  }

  $self->SUPER::start();
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  $self->startManagedVDO();
  $self->SUPER::activate();
}

########################################################################
# Start a managed VDO
##
sub startManagedVDO {
  my ($self) = assertNumArgs(1, @_);
  my $affinityList = $self->{vdoAffinityList};
  if (defined($affinityList)) {
    if (ref($affinityList) eq 'ARRAY') {
      $affinityList = join(',', @{$affinityList});
    }
    # If one of the comma-separated components includes an "@", it's
    # detailing affinity for specific kernel threads, as is every
    # component after it, so drop the rest.
    $affinityList =~ s/,[^,]*@.*$//;
  }

  my $extraArgs = {
                   cpuAffinityList => $affinityList,
                   cpuAffinityMask => $self->{vdoAffinity},
                  };
  if (defined($self->{_forceRebuild})) {
    $extraArgs->{forceRebuild} = 1;
    delete $self->{_forceRebuild};
  }
  $self->assertVDOCommand("start", $extraArgs);
}

########################################################################
# @inherit
##
sub stopVDO {
  my ($self) = assertNumArgs(1, @_);
  $self->assertVDOCommand("stop");
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);

  delayFailures(sub { $self->doRemove(); },
                sub { $self->SUPER::teardown(); });
}

########################################################################
# @inherit
##
sub migrate {
  my ($self, $newMachine) = assertNumArgs(2, @_);
  my $currentHost = $self->getMachineName();
  my $newHost = $newMachine->getName();
  if ($currentHost eq $newHost) {
    $self->installModule();
    return;
  }

  my $migrate = sub {
    $log->info("Migrating VDO device from $currentHost to $newHost");
    $self->SUPER::migrate($newMachine);
    $self->installModule();
    $self->registerFileCleanup();

    if ($newHost ne $self->getStorageHost()) {
      # If we are building on an ISCSI device, we have to import the VDO every
      # time. This means removing the confFile also.
      $self->getMachine()->runSystemCmd("sudo rm -f $self->{confFile}");
      $self->doImport(activate => 'disabled');
      $self->assertVDOCommand("activate");
    }
  };
  $self->runWhileStopped($migrate);
}

########################################################################
# @inherit
##
sub growLogical {
  my ($self, $logicalSize) = assertNumArgs(2, @_);
  $self->assertVDOCommand("growLogical",
                          { vdoLogicalSize => sizeToLvmText($logicalSize) });
  $self->{logicalSize} = $logicalSize;
}

########################################################################
# @inherit
##
sub growPhysical {
  my ($self, $physicalSize) = assertNumArgs(2, @_);
  $self->resizeStorageDevice($physicalSize);
  $self->assertVDOCommand("growPhysical");
  $self->{physicalSize} = $physicalSize;
}

########################################################################
# @inherit
##
sub forceRebuild {
  my ($self) = assertNumArgs(1, @_);
  $self->{_forceRebuild} = 1;
  $self->{expectIndexer} = 1;
  $self->recover();
}

########################################################################
# Get the logical size of the kvdo volume.
##
sub getVDOSize {
  my ($self) = assertNumArgs(1, @_);

  $self->runVDOCommand('status', { verbose => 0 });
  my $status = yamlStringToHash($self->getMachine()->getStdout());
  my $vdos = $status->{'VDOs'};
  foreach my $vdoName (keys(%{$vdos})) {
    if ($vdoName eq $self->{deviceName}) {
      return $vdos->{$vdoName}->{'Logical size'};
    }
  }
  confess("Could not determine VDO logical size");
}

########################################################################
# Get the physical size of the kvdo volume.
##
sub getVDOPhysicalSize {
  my ($self) = assertNumArgs(1, @_);

  $self->runVDOCommand('status', { verbose => 0 });
  my $status = yamlStringToHash($self->getMachine()->getStdout());
  my $vdos = $status->{'VDOs'};
  foreach my $vdoName (keys(%{$vdos})) {
    if ($vdoName eq $self->{deviceName}) {
      return $vdos->{$vdoName}->{'Physical size'};
    }
  }
  confess("Could not determine VDO physical size");
}

########################################################################
# Assert that a vdo manager command was successful.
#
# @param  command    the command to run
# @oparam extraArgs  additional arguments to the command
##
sub assertVDOCommand {
  my ($self, $command, $extraArgs) = assertMinMaxArgs([{}], 2, 3, @_);
  $self->runVDOCommand($command, $extraArgs, 0);
}

########################################################################
# Hook/factory method to construct a vdo manager command string.
#
# @oparam args  the arguments to the command
#
# @return a new C<Permabit::CommandString::VDO>
##
sub makeVDOCommandString {
  my ($self, $args) = assertNumArgs(2, @_);
  return Permabit::CommandString::VDO->new($self, $args);
}

########################################################################
# Make a vdo manager command with arguments for this managed device.
#
# @param  command    the command to run
# @oparam extraArgs  additional arguments to the command
##
sub makeVDOCommand {
  my ($self, $command, $extraArgs) = assertMinMaxArgs([{}], 2, 3, @_);

  my $args = {
              command      => $command,
              doSudo       => 1,
              logfile      => $self->{managerLogFile},
              verbose      => 1,
              %$extraArgs,
             };
  if ($command eq 'create' || $command eq 'import') {
    # --device is a required parameter for create and import commands.
    # Normally that value will come from the call below. However, in order
    # to test the scenario where --device is not supplied, we check here
    # to see if the argument has been passed in already. If it has,
    # then we do not override it with the call. This will allow us
    # to pass in undef as a value.
    if (!exists($args->{device})) {
      $args->{device} = $self->getStoragePath();
    }
  }
  if (defined($self->{blockMapCacheSize})) {
    $args->{blockMapCacheSize} = sizeToLvmText($self->{blockMapCacheSize});
  }

  return $self->makeVDOCommandString($args);
}

########################################################################
# Run a vdo manager command.
#
# @param  command    the command to run
# @oparam extraArgs  additional arguments to the command
# @oparam okToFail   do or do not allow failure
##
sub runVDOCommand {
  my ($self, $command, $extraArgs, $okToFail)
    = assertMinMaxArgs([{}, 1], 2, 4, @_);

  my $machine = $self->getMachine();
  my $vdoCommand = $self->makeVDOCommand($command, $extraArgs);
  if ($okToFail) {
    $machine->executeCommand("($vdoCommand)");
  } else {
    $machine->assertExecuteCommand("($vdoCommand)");
  }
}

########################################################################
# Wait for the device to be ready (typically after a reboot).
#
# @oparam timeout  the timeout in seconds, defaults to 5 minutes
##
sub waitForDeviceReady {
  my ($self, $timeout) = assertMinMaxArgs(1, 2, @_);
  my $devName = $self->getSymbolicPath();
  $timeout //= 5 * $MINUTE;
  # TODO Update the codebase to set the timeout via the machine type
  # as opposed to the setting here
  $log->info("Waiting up to $timeout seconds for VDO $devName to be usable");
  # Add in additional start up sequence since VDO could be on top
  # of two layers of LVM thus needing extra steps after start up.
  my $moduleName = $self->getModuleName();
  my $cmd = "test -b $devName ";
  $cmd .= "|| (sudo vgscan && sudo vgchange -a y ";
  $cmd .= "&& test -b $devName)";

  my $sub = sub {
    return $self->getMachine()->sendCommand($cmd) == 0;
  };
  retryUntilTimeout($sub, "VDO $devName did not start", $timeout);
}

########################################################################
# Retrieve the VDO's configuration file contents.
# @return   the config file contents or undef if $self->{confFile} is not
#           defined
##
sub getConfigurationContents {
  my ($self) = assertNumArgs(1, @_);
  if (!defined($self->{confFile})) {
    return undef;
  }
  my $result = $self->getMachine()->cat($self->{confFile});
  return $result;
}

########################################################################
# Overwrites the VDO's configuration file contents.
#
# If $self->{confFile} is not defined this is a no-op.
##
sub saveConfigurationContents {
  my ($self, $configContents) = assertNumArgs(2, @_);
  if (!defined($self->{confFile})) {
    return;
  }

  $self->runOnHost("echo \"$configContents\" | sudo tee $self->{confFile}");
}

########################################################################
# Retrieve a VDO configuration datum by name.
# @param key        the name of the configuration datum
# @return           the datum, or undef if $self->{confFile} is not
#                   defined, or there's no VDO device with our name, or
#                   the tag is not found
##
sub getConfigurationDatum {
  my ($self, $key) = assertNumArgs(2, @_);

  my $configContents = $self->getConfigurationContents();
  if (!defined($configContents)) {
    return undef;
  }

  my $config = YAML::Load($configContents)->{config};
  my $datum = $config->{vdos}->{$self->{deviceName}}->{$key};

  # If the datum is a hash the actual value we want is the "value" entry.
  if (defined($datum) && (ref($datum) eq "HASH")) {
    $datum = $datum->{value};
  }
  return $datum;
}

########################################################################
# Disable the compression on a VDO device
##
sub disableCompression {
  my ($self) = assertNumArgs(1, @_);
  $self->assertVDOCommand("disableCompression");
}

########################################################################
# Disable the deduplication on a VDO device
##
sub disableDeduplication {
  my ($self) = assertNumArgs(1, @_);
  $self->assertVDOCommand("disableDeduplication");
}

########################################################################
# Enable the compression on a VDO device
##
sub enableCompression {
  my ($self) = assertNumArgs(1, @_);
  $self->assertVDOCommand("enableCompression");
}

########################################################################
# Enable the deduplication on a VDO device
##
sub enableDeduplication {
  my ($self) = assertNumArgs(1, @_);
  $self->assertVDOCommand("enableDeduplication");
}

########################################################################
# @inherit
##
sub isVDOCompressionEnabled {
  my ($self) = assertNumArgs(1, @_);
  my $compression = $self->SUPER::isVDOCompressionEnabled();

  # make sure the runtime value matches the value in the config file
  my $confVal = $self->getConfigurationDatum("compression");
  my $desiredSetting = $compression ? "enabled" : "disabled";
  assertEq($desiredSetting, $confVal);

  return $compression;
}

########################################################################
# @inherit
##
sub isVDODedupeEnabled {
  my ($self) = assertNumArgs(1, @_);
  my $deduplication = $self->SUPER::isVDODedupeEnabled();

  # make sure the runtime value matches the value in the config file
  my $confVal = $self->getConfigurationDatum("deduplication");
  my $desiredSetting = $deduplication ? "enabled" : "disabled";
  assertEq($desiredSetting, $confVal);

  return $deduplication;
}

1;
