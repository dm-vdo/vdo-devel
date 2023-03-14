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
  assertDefined
  assertNENumeric
  assertNumArgs
  assertTrue
);
use Permabit::SupportedVersions qw(
  $SUPPORTED_SCENARIOS
  getDirectUpgradesToVersion
);

use base qw(VDOTest::UpgradeBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple Audit VDOs, as some upgrades require rebuilds
   auditVDO   => 1,
   # @ple Number of blocks to write
   blockCount => 1000,
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
  my ($package, $startingScenario, $endingScenario) = assertNumArgs(3, @_);
  my $name = join('', $package, "::testDirtyUpgrade",
                  ucfirst($startingScenario), "To", ucfirst($endingScenario));
  # Remove dots and dashes from the testname, as those are invalid.
  $name =~ s/[.-]/_/g;
  my $test = $package->make_test_from_coderef(\&_runDirtyUpgradeTest, $name);
  $test->{initialScenario} = $startingScenario;
  $test->{_upgradeScenario} = $endingScenario;
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
    # Find the scenario corresponding to this version and add the test
    my $scenario = undef;
    foreach my $name (keys(%{$SUPPORTED_SCENARIOS})) {
      if ($SUPPORTED_SCENARIOS->{$name}{moduleVersion} eq $version) {
        $scenario = $name;
        last;
      }
    }
    assertDefined($scenario, "Version '$version' must be associated with a"
                  . " scenario in SUPPORTED_SCENARIOS in order to be used.");
    $suite->add_test(generateOneStepTest($package, $scenario, "X86_RHEL9_head"));
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

  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $scenario = $self->generateScenarioHash($self->{_upgradeScenario});
  $machine->emergencyRestart();
  $device->runVDOCommand("stop");
  $device->switchToScenario($scenario);

  # Start the VDO and expect a failure.
  my $kernelCursor = $machine->getKernelJournalCursor();
  $device->runVDOCommand("start");
  assertNENumeric(0, $machine->getStatus());
  assertTrue($machine->searchKernelJournalSince($kernelCursor,
                                                "VDO_UNSUPPORTED_VERSION"));

  # Switch back to the initial version for test cleanup.
  $scenario = $self->generateScenarioHash($self->{initialScenario});
  $device->switchToScenario($scenario);

  # Manually start the VDO with the initial version to ensure it still works.
  $device->recover();

  # Verify that this is a direct upgrade that would have worked if not dirty.
  $device->stop();
  my $upgradeVersion =
    $SUPPORTED_SCENARIOS->{$self->{_upgradeScenario}}{moduleVersion};
  if ($device->needsExplicitUpgrade($upgradeVersion)) {
    $device->upgrade($upgradeVersion);
  } else {
    $scenario = $self->generateScenarioHash($self->{_upgradeScenario});
    $device->switchToScenario($scenario);
  }
  $device->start();
}

1;
