##
# Provide a basic test for upgrading, which can be extended with specific
# versions.
#
# $Id$
##
package VDOTest::UpgradeBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertMinMaxArgs
  assertNumArgs
);

use base qw(VDOTest::MigrationBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# Make a test with a given name and version list.
#
# @param  package   The package in which to make the test
# @param  name      The name of the test
# @param  versions  The VDO versions the test should go through
#
# @return The requested test
##
sub makeTest {
  my ($package, $name, $versions) = assertNumArgs(3, @_);
  my $test = $package->make_test_from_coderef(\&_runUpgradeTest,
                                              "${package}::${name}");
  $test->{initialScenario}      = { version => shift(@$versions) };
  $test->{intermediateVersions} = $versions;
  return $test;
}

#############################################################################
# Do an upgrade by using the upgrader script.
##
sub doUpgrade {
  my ($self, $newVersion) = assertNumArgs(2, @_);

  my $device = $self->getDevice();
  $device->upgrade($newVersion);
  $device->waitForIndex();
}

#############################################################################
# Do an upgrade by swapping out the binaries.
##
sub _switchToIntermediateVersion {
  my ($self, $intermediateVersion) = assertNumArgs(2, @_);
  my $device = $self->getDevice();
  $device->stop();
  $device->switchToScenario({ version => $intermediateVersion });
  $device->start();
}

#############################################################################
# Test basic read/write and dedupe capability after a series of upgrades.
#
# @oparam  dontVerify  don't run the final verification step
##
sub _runUpgradeTest {
  my ($self, $dontVerify) = assertMinMaxArgs([0], 1, 2, @_);
  my @upgradeList = @{$self->{intermediateVersions}};
  push(@upgradeList, "head");

  my $upgrades = join(" to ", @upgradeList);
  $log->info("Test upgrading $self->{initialScenario}{version} to $upgrades");

  $self->establishStartingDevice();
  my $device = $self->getDevice();
  foreach my $intermediateVersion (@upgradeList) {
    if ($device->needsExplicitUpgrade($intermediateVersion)) {
      $log->info("Upgrading to $intermediateVersion explicitly");
      $self->doUpgrade($intermediateVersion);
    } else {
      $log->info("Upgrading to $intermediateVersion implicitly");
      $self->_switchToIntermediateVersion($intermediateVersion);
    }
  }
  if (!$dontVerify) {
    $self->verifyFinalState();
  }

  $log->info("Returning to $self->{initialScenario}{version}");
  $self->_switchToIntermediateVersion($self->{initialScenario}{version});
  # XXX  We should check that the old version still works.
}

1;
