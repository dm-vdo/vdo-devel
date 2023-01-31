##
# Provide a basic framework for migrating from one machine to another,
# in order to test changes to hardware, platform, or VDO version.
#
# $Id$
##
package VDOTest::MigrationBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertMinMaxArgs
  assertNumArgs
);
use Permabit::Constants;
use Permabit::Utils qw(ceilMultiple);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple Audit VDO metadata
   auditVDO   => 1,
   # @ple Number of blocks to write
   blockCount => 33000,
   # @ple Use an upgrade device backed by iscsi, and don't use stripfua
   deviceType => "upgrade-iscsi-linear",
   # @ple Data to share between test methods
   _testData  => undef,
  );
##

#############################################################################
# Test basic read/write and dedupe capability before a migration.
##
sub setupDevice {
  # XXX Placeholder for future change
  return 1;
}

#############################################################################
# Test basic read/write and dedupe capability after a migration.
##
sub verifyDevice {
  # XXX Placeholder for future change
  return 1;
}

#############################################################################
# Move the VDO device to a new scenario.
##
sub switchToIntermediateScenario {
  my ($self, $scenario) = assertNumArgs(2, @_);
  my $device = $self->getDevice();
  $device->stop();
  $device->switchToScenario($scenario);
  $device->start();
}

#############################################################################
# Test basic read/write and dedupe capability after a series of upgrades.
#
# @oparam  dontVerify  don't run the final verification step
##
sub _runTest {
  my ($self, $dontVerify) = assertMinMaxArgs([0], 1, 2, @_);
  my @upgradeList = @{$self->{intermediateVersions}};
  push(@upgradeList, "head");

  my $upgrades = join(" to ", @upgradeList);
  $log->info("Test upgrading $self->{initialScenario}{version} to $upgrades");

  $self->setupDevice();
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
    $self->verifyDevice();
  }

  $log->info("Returning to $self->{initialScenario}{version}");
  $self->_switchToIntermediateVersion($self->{initialScenario}{version});
  # XXX  We should check that the old version still works.
}

1;
