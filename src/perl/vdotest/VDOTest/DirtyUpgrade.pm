##
# Verify that VDO cannot be upgraded when a recovery is needed.
#
# $Id$
##
package VDOTest::DirtyUpgrade;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertNENumeric
  assertNumArgs
  assertTrue
);
use Permabit::SupportedVersions qw(getDirectUpgradesToVersion);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple Audit VDOs, as some upgrades require rebuilds
   auditVDO   => 1,
   # @ple Number of blocks to write
   blockCount => 1000,
   # @ple Use a VDOUpgrade device
   deviceType => "upgrade",
  );
##

#############################################################################
# Generate a test from one version to the next.
#
# @param package        The package to make the test in
# @param startingBuild  The version to start with
# @param endingBuild    The version to (attempt to) upgrade to
#
# @return A test in the package.
##
sub generateOneStepTest {
  my ($package, $startingBuild, $endingBuild) = assertNumArgs(3, @_);

  my $name = join('', $package, "::testDirtyUpgrade",
                  ucfirst($startingBuild), "To", ucfirst($endingBuild));
  # Remove dots and dashes from the testname, as those are invalid.
  $name =~ s/[.-]/_/g;
  my $test = $package->make_test_from_coderef(\&_runDirtyUpgradeTest, $name);
  $test->{initialScenario} = { version => $startingBuild };
  $test->{_upgradeVersion} = $endingBuild;
  return $test;
}

#############################################################################
# Construct a suite testing all upgrades that shouldn't work when dirty.
##
sub suite {
  my ($package) = assertNumArgs(1, @_);
  my $suite = Test::Unit::TestSuite->empty_new($package);

  # Generate a test of every valid upgrade to head (if this is on the mainline)
  # or to the current version of this branch (if on a release branch).
  foreach my $version (getDirectUpgradesToVersion("head")) {
    $suite->add_test(generateOneStepTest($package, $version, "head"));
  }

  return $suite;
}

#############################################################################
# Test that an upgrade while the device is dirty does not work.
##
sub _runDirtyUpgradeTest {
  my ($self) = assertNumArgs(1, @_);

  # Write some data.
  my $data = $self->createSlice(blockCount => $self->{blockCount});
  $data->write(tag => "data", fsync => 1);

  my $device  = $self->getDevice();
  my $machine = $device->getMachine();
  $machine->emergencyRestart();
  $device->runVDOCommand("stop");
  $device->switchToVersion($self->{_upgradeVersion});

  # Start the VDO and expect a failure.
  my $kernelCursor = $machine->getKernelJournalCursor();
  $device->runVDOCommand("start");
  assertNENumeric(0, $machine->getStatus());
  assertTrue($machine->searchKernelJournalSince($kernelCursor,
                                                "VDO_UNSUPPORTED_VERSION"));

  # Switch back to the initial version for test cleanup.
  $device->switchToScenario($self->{initialScenario});

  # Manually start the VDO with the initial version to ensure it still works.
  $device->recover();

  # Verify that this is a direct upgrade that would have worked if not dirty.
  $device->stop();
  if ($device->needsExplicitUpgrade($self->{_upgradeVersion})) {
    $device->upgrade($self->{_upgradeVersion});
  } else {
    $device->switchToVersion($self->{_upgradeVersion});
  }
  $device->start();
}

1;
