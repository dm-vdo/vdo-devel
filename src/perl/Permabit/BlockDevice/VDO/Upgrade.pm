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
use Permabit::SupportedVersions qw($SUPPORTED_VERSIONS);

use base qw(Permabit::BlockDevice::VDO::Managed);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
#
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple The path to the currently installed VDO
     _currentInstall  => undef,
     # @ple Run the upgrader in verbose mode
     _upgraderVerbose => 1,
     # @ple A hash tracking files for different versions
     _versions        => {},
     # @ple A hash taken from SUPPORTED_VERSIONS
     _versionData     => undef,
    );
##

########################################################################
# @paramList{inherited}
#
our %BLOCKDEVICE_INHERITED_PROPERTIES
  = (
     # @ple version of VDO to setup.  Must be a key of SUPPORTED_VERSIONS.
     setupVersion => undef,
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
  assertDefined($self->{setupVersion});
}

########################################################################
# Set the _versionData from the key
#
# @param versionKey  key into the SUPPORTED_VERSIONS hash
##
sub _setupVersionData {
  my ($self, $versionKey) = assertNumArgs(2, @_);

  assertDefined($SUPPORTED_VERSIONS->{$versionKey});
  $self->{_versionData} = $SUPPORTED_VERSIONS->{$versionKey};
}

########################################################################
# @inherit
##
sub setup {
  my ($self) = assertNumArgs(1, @_);
  $log->info("Using VDO version $self->{setupVersion}");
  $self->switchToVersion($self->{setupVersion});
  $self->SUPER::setup();
  $self->verifyModuleVersion();
}

########################################################################
# Start a managed VDO
##
sub startManagedVDO {
  my ($self) = assertNumArgs(1, @_);
  # We need to suppress checking for blocked tasks, because the Fluorine
  # release still has blocked tasks reported during "dmsetup create".
  my $start = sub { $self->SUPER::startManagedVDO(); };
  $self->getMachine()->withKernelLogErrorCheckDisabled($start, "blocked");
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

  # If the version has not been set yet, don't install anything.
  if (!defined($self->{_versionData})) {
    return;
  }

  # Set module properties to use the correct version.
  $self->setModuleVersion($self->{_versionData}->{moduleVersion});
  $self->setModuleSourceDir($self->{_currentInstall});
  $self->SUPER::installModule();
}

########################################################################
# @inherit
##
sub uninstallModule {
  my ($self, $machineName) = assertMinMaxArgs(1, 2, @_);

  # If the version has not been set yet, don't uninstall anything.
  if (!defined($self->{_versionData})) {
    return;
  }

  $self->SUPER::uninstallModule($machineName);
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
# run the requested version. This includes building the proper kernel
# binary, building the user tools, and setting up the proper directory
# structures for python.
#
# @param versionName  The name of the version
#
# @return The path to the directory containing the new version files
##
sub _createVersionedVDODirectory {
  my ($self, $versionName) = assertNumArgs(2, @_);
  my $versionDir           = makeFullPath($self->{workDir}, $versionName);
  $self->runOnHost("mkdir $versionDir");

  my $arch = $self->runOnHost("uname -m", 1);
  chomp($arch);

  # We only need to handle RPMs here because all release versions are
  # released as RPMs, and head is handled separately. There can be
  # only one of each VDO RPM in the release path.
  my $releasePath = $self->{_versionData}->{path};
  my $sources     = "*vdo-$versionName*.src.rpm";
  $self->runOnHost("cp -p $releasePath/$sources $versionDir");

  # Build the kernel module from source and move the result to the top level.
  # The KCFLAGS argument allow us to build vdo-6.2 (Aluminum) RPMs on Fedora.
  $log->debug("Building kernel module RPM");
  $self->runOnHost(["KCFLAGS='-Wno-vla' rpmbuild --rebuild"
                    . " --define='_topdir $versionDir'"
                    . " $versionDir/*kvdo-$versionName*.src.rpm",
                    "mv -f $versionDir/RPMS/$arch/* $versionDir"]);

  # Build the user tools from source, and unpack them to the top level.
  $log->debug("Building and unpacking user tools RPM");
  my $rpmbuild = join(' ',
                      "rpmbuild --rebuild",
                      " --define='_topdir $versionDir'",
                      " --define='_bindir /'",
                      " $versionDir/vdo-$versionName*.src.rpm");
  my $unpack = join(' && ',
                    "cd $versionDir",
                    "rpm2cpio RPMS/$arch/vdo-$versionName*.rpm | cpio -id");
  my $pythonPath = $self->getMachine()->getPythonLibraryPath();
  my $link = "ln -s $versionDir/$pythonPath $versionDir/pythonlibs";
  $self->runOnHost([$rpmbuild, $unpack, $link]);

  return $versionDir;
}

########################################################################
# Get the path to a directory of artifacts for a specific version of
# VDO. This creates the directory, if necessary.
#
# @return The path to the VDO artifacts of the specified version
##
sub _getVersionedVDODirectory {
  my ($self) = assertNumArgs(1, @_);

  my $versionName = $self->{_versionData}->{moduleVersion};
  if ($self->{_versionData}->{isCurrent}
      && $self->{_versionData}->{branch} eq "head") {
    $self->{_versions}->{$versionName} = $self->{binaryDir};
  }

  if (defined($self->{_versions}->{$versionName})) {
    return $self->{_versions}->{$versionName};
  }

  my $versionDir = $self->_createVersionedVDODirectory($versionName);
  $self->{_versions}->{$versionName} = $versionDir;
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
  my $modinfo = YAML::Load($self->runOnHost("sudo modinfo kvdo"));
  return $modinfo->{version};
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
# @param version  The VDO version to switch to
##
sub switchToVersion {
  my ($self, $version) = assertNumArgs(2, @_);
  $self->uninstallModule();
  $log->debug("Switching to VDO version $version");

  $self->_setupVersionData($version);
  $self->_installBinaries();
  $self->installModule();
  $self->verifyModuleVersion();
}

########################################################################
# Run the vdoUpgrader command appropriate for a given version.
#
# XXX The explicit upgrade commands are not currently supported.
#
# @param  newVersion  The VDO version to upgrade to
# @oparam extraArgs   Any extra arguments to the upgrade command
##
sub runVDOUpgrader {
  my ($self, $newVersion, $extraArgs) = assertMinMaxArgs([""], 2, 3, @_);
  $log->debug("Stopping VDO version $self->{_versionData}->{moduleVersion}");

  $self->_setupVersionData($newVersion);
  my $newVersionPath = $self->_getVersionedVDODirectory();

  my $verbose = $self->{_upgraderVerbose} ? " --verbose" : "";

  my $upgrader = makeFullPath($newVersionPath, "vdoUpgrader.sh");
  my $upgradeCmd = "sudo $upgrader $verbose"
                   . " --albireoBinaryPath=$self->{_currentInstall}"
                   . " --moduleSourceDir=$newVersionPath"
                   . " --confFile=$self->{confFile} $extraArgs";
  my $upgradeSub = sub { $self->runOnHost($upgradeCmd); };
  $self->getMachine()->withKernelLogErrorCheckDisabled($upgradeSub, "blocked");
  $self->verifyModuleVersion();
}

########################################################################
# Use the vdo manager option to explicitly upgrade an installed VDO to
# a different version.
#
# XXX The explicit upgrade command is not currently supported.
#
# @param  newVersion  The VDO version to upgrade to
# @oparam extraArgs   Any extra arguments to the upgrade command
##
sub upgrade {
  my ($self, $newVersion, $extraArgs) = assertMinMaxArgs([""], 2, 3, @_);

  $self->stop();
  $self->runVDOUpgrader($newVersion, $extraArgs);
  $self->start();
}

########################################################################
# @inherit
##
sub makeVDOCommandString {
  my ($self, $args) = assertNumArgs(2, @_);
  my $params = {
                albireoBinaryPath => "$self->{_currentInstall}",
                binary            => makeFullPath($self->{_currentInstall},
                                                  "vdo"),
                pythonLibDir      => "$self->{_currentInstall}/pythonlibs",
                %$args,
               };
  return $self->SUPER::makeVDOCommandString($params);
}

########################################################################
# @inherit
##
sub makeVDOStatsCommandString {
  my ($self, $args) = assertNumArgs(2, @_);
  my $params = {
                albireoBinaryPath => "$self->{_currentInstall}",
                binary            => makeFullPath($self->{_currentInstall},
                                                  "vdostats"),
                pythonLibDir      => "$self->{_currentInstall}/pythonlibs",
                %$args,
               };
  return $self->SUPER::makeVDOStatsCommandString($params);
}

1;
