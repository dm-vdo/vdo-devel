##
# Perl object that represents a VDO device that can perform upgrades.
#
# $Id$
##
package Permabit::BlockDevice::VDO::Upgrade;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertMinMaxArgs
  assertNumArgs
  assertRegexpMatches
  assertType
);
use Permabit::Constants qw($GB);
use Permabit::Utils qw(makeFullPath);
use Permabit::SupportedVersions qw($SUPPORTED_SCENARIOS $SUPPORTED_VERSIONS);

use base qw(Permabit::BlockDevice::VDO::LVMManaged);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
#
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple Skip setting up the device on creation
     setupOnCreation   => 0,
     # @ple The path to the currently installed VDO
     _currentInstall   => undef,
     # @ple A hash taken from SUPPORTED_SCENARIOS
     _scenarioData     => undef,
     # @ple Run the upgrader in verbose mode
     _upgraderVerbose  => 1,
     # @ple A hash taken from SUPPORTED_VERSIONS
     _versionData      => undef,
    );
##

########################################################################
# @paramList{inherited}
#
our %BLOCKDEVICE_INHERITED_PROPERTIES
  = (
     # @ple The initial VDO scenario
     initialScenario => {},
    );
##

########################################################################
# @inherit
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2, @_);
  # Setting the metadataSize here is a hack to get physicalSize to work.
  # Ultimately, the size of the underlying device should not rely on any
  # properties of a particular VDO version.
  $arguments->{metadataSize} //= 11 * $GB;
  $self->SUPER::configure($arguments);
}

########################################################################
# Set the scenario and version data structures from the scenario key
#
# @param scenarioKey  Key into the SUPPORTED_SCENARIOS hash
##
sub _setupData {
  my ($self, $scenarioKey) = assertNumArgs(2, @_);

  assertDefined($SUPPORTED_SCENARIOS->{$scenarioKey});
  $self->{_scenarioData} = $SUPPORTED_SCENARIOS->{$scenarioKey};

  my $versionKey = $self->{_scenarioData}->{moduleVersion};
  assertDefined($SUPPORTED_VERSIONS->{$versionKey});
  $self->{_versionData} = $SUPPORTED_VERSIONS->{$versionKey};
}

########################################################################
# @inherit
##
sub getCurrentVDOStats {
  my ($self, $class) = assertMinMaxArgs(1, 2, @_);
  my @args = ();
  if (defined($class)) {
    push @args, $class;
  } else {
    push @args, $self->{_versionData}->{statistics};
  }
  return $self->SUPER::getCurrentVDOStats(@args);
}

########################################################################
# @inherit
##
sub installModule {
  my ($self) = assertNumArgs(1, @_);

  # If the data structures have not been set yet, don't install anything.
  if (!defined($self->{_scenarioData}) || !defined($self->{_versionData})) {
    return;
  }

  # Install the binary files on the host, if necessary. This is needed for
  # migration tests when the VDO version is the same but the machine
  # architecture or OS varies between scenarios.
  $self->_installBinaries();

  # Set module properties to use the correct version.
  $self->setModuleVersion($self->{_versionData}->{moduleVersion});
  $self->setModuleSourceDir($self->{_currentInstall});
  $self->SUPER::installModule();
}

########################################################################
# Determine whether a given version of VDO requires an explicit upgrade step
# from the device's current version.
#
# XXX Currently there is no explicit upgrade implemented.
#
# @param newVersion  The new VDO version
#
# @return true if an explicit upgrade is needed
##
sub needsExplicitUpgrade {
  my ($self, $newVersion) = assertNumArgs(2, @_);
  return 0;
}

########################################################################
# Create a directory containing all artifacts required to install and
# run the requested version.
#
# @param  versionName  The name of the version
# @param  versionDir   The versioned directory to create
##
sub _createVersionedVDODirectory {
  my ($self, $versionName, $versionDir) = assertNumArgs(3, @_);
  $self->runOnHost("mkdir -p $versionDir");

  # We only need to handle RPMs here because all release versions are released
  # as RPMs. There can be only one of each VDO RPM in the release path.
  my $releasePath;
  if ($self->{_versionData}->{branch} eq "head") {
    $releasePath = $self->{binaryDir};
  } else {
    $releasePath = $self->{_versionData}->{path};
  }
  my $sources     = "*vdo-$versionName*.src.rpm";
  $self->runOnHost("cp -p $releasePath/$sources $versionDir");
}

########################################################################
# Build the kernel and user tools modules for the specified version.
#
# @param versionName  The name of the version
# @param versionDir   The versioned VDO directory where build artifacts exist
##
sub _buildModules {
  my ($self, $versionName, $versionDir) = assertNumArgs(3, @_);
  my $arch = $self->runOnHost("uname -m", 1);
  chomp($arch);

  # Build the kernel module from source and move the result to the top level.
  # The KCFLAGS argument allows us to build vdo-6.2 (Aluminum) RPMs on Fedora.
  $log->debug("Building kernel module SRPM");
  my $kernelBuild = join(' ',
                         "KCFLAGS='-Wno-vla' rpmbuild --rebuild",
                         "--define='_topdir $versionDir'",
                         "$versionDir/*kvdo-$versionName*.src.rpm");
  my $move = "mv -f $versionDir/RPMS/$arch/* $versionDir";
  $self->runOnHost([$kernelBuild, $move]);

  # Build the user tools from source, and move the necessary RPMs to the top
  # level.
  $log->debug("Building user tools SRPM");
  my $userBuild = join(' ',
                       "rpmbuild --rebuild",
                       "--define='_topdir $versionDir'",
                       "--define='_bindir $self->{userBinaryDir}'",
                       "$versionDir/vdo-$versionName*.src.rpm");
  my $userMove = "mv -f $versionDir/RPMS/$arch/vdo-$versionName"
                  . "*.rpm $versionDir";
  $self->runOnHost([$userBuild, $userMove]);

  my $supportMove = "mv -f $versionDir/RPMS/$arch/vdo-support-$versionName*.rpm"
                     . " $versionDir";
  $self->runOnHost($supportMove);

  return $versionDir;
}

########################################################################
# Get the path to a directory of artifacts for a specific version of
# VDO and host. This creates the directory, if necessary.
#
# @return The path to the VDO artifacts of the specified version and host
##
sub _getVersionedVDODirectory {
  my ($self) = assertNumArgs(1, @_);
  my $hostName = $self->getMachineName();
  my $versionName = $self->{_versionData}->{moduleVersion};
  my $versionDir = makeFullPath($self->{workDir}, $versionName);

  # Create the versioned VDO directory if it does not exist on this host.
  if ($self->sendCommand("test -d $versionDir")) {
    $self->_createVersionedVDODirectory($versionName, $versionDir);
  }

  # Build or rebuild the modules if necessary
  my $scenarioArch = lc($self->{_scenarioData}{arch});
  my @versionArchRPMNames =
    map { "$versionDir/$_" } ("*kvdo-$versionName*$scenarioArch.rpm",
                              "vdo-$versionName*$scenarioArch.rpm");

  foreach my $name (@versionArchRPMNames) {
    my $errno = $self->sendCommand("test -f $name");
    if ($errno != 0) {
      $log->debug("Building the VDO modules for architecture $scenarioArch");
      $self->_buildModules($versionName, $versionDir);
      last;
    }
  }

  return $versionDir;
}

########################################################################
# "Install" the user binaries by changing the current install directory.
##
sub _installBinaries {
  my ($self) = assertNumArgs(1, @_);
  $self->{_currentInstall} = $self->_getVersionedVDODirectory();
}

########################################################################
# Get the version from VDO.
##
sub getInstalledVersion {
  my ($self)  = assertNumArgs(1, @_);
  my $version = $self->runOnHost("sudo modinfo kvdo -F version");
  chomp($version);
  return $version;
}

########################################################################
# Verifies the installed module has the same version as expected.
##
sub verifyModuleVersion {
  my ($self) = assertNumArgs(1, @_);

  my $versionString = $self->getInstalledVersion();
  if (!$self->{_versionData}->{isCurrent}) {
    assertEq($versionString, $self->{_versionData}->{moduleVersion});
    return;
  }

  # Only match as many fields of the module version as the branch version has.
  assertRegexpMatches(qr/^\Q$self->{_versionData}->{moduleVersion}./,
                      $versionString);
}

########################################################################
# Switch to use a different VDO version without using vdoUpgrader. The device
# must be stopped, or uninstalling the old module will complain that it is
# still in use.
#
# @param scenario  A hashref describing the new version and machine
##
sub switchToScenario {
  my ($self, $scenario) = assertNumArgs(2, @_);
  my $machine = $scenario->{machine} // $self->getMachine();
  my $version = $scenario->{version} // "head";

  $log->info("Switching to VDO $version on " . $machine->getName());
  $self->_setupData($scenario->{name});
  $self->migrate($machine);
  $self->verifyModuleVersion();
}

########################################################################
# @inherit
##
sub makeVDOStatsCommandString {
  my ($self, $args) = assertNumArgs(2, @_);
  my $params = {
                binary => $self->getMachine()->findNamedExecutable("vdostats"),
                %$args,
               };
  return $self->SUPER::makeVDOStatsCommandString($params);
}

1;
