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
  assertDefined
  assertMinMaxArgs
  assertNumArgs
);
use Permabit::RSVPer;
use Permabit::SupportedVersions qw($SUPPORTED_SCENARIOS $SUPPORTED_VERSIONS);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple Audit VDO metadata
   auditVDO   => 1,
   # @ple Number of blocks to write
   blockCount => 256,
   # @ple Use an upgrade device backed by iscsi, and don't use stripfua
   deviceType => "upgrade-iscsi-linear",
   # @ple Hashref mapping scenario keys to the scenario hash to be used
   _scenarios => undef,
   # @ple Data to share between test methods
   _testData  => [],
  );
##

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  my @scenarioNames = ($self->{initialScenario},
                       @{$self->{intermediateScenarios}});

  # Verify the test scenarios and versions are valid.
  foreach my $name (@scenarioNames) {
    assertDefined($SUPPORTED_SCENARIOS->{$name}, "Scenario '$name' must be"
                . " defined in SUPPORTED_SCENARIOS to be used.");

    my $versionName = $SUPPORTED_SCENARIOS->{$name}{moduleVersion};
    assertDefined($SUPPORTED_VERSIONS->{$versionName}, "Version '$versionName'"
                . " must be defined in SUPPORTED_VERSIONS to be used.");
  }

  # Generate the scenario hashes and reserve the needed hosts.
  foreach my $name (@scenarioNames) {
    $self->{_scenarios}{$name} = $self->generateScenarioHash($name);
  }

  $self->SUPER::set_up();

  # Add the userMachines to the scenario hashes.
  foreach my $name (@scenarioNames) {
    my $scenario = $self->{_scenarios}->{$name};
    $scenario->{machine} = $self->getUserMachine($scenario->{hostname});
  }

  # Install the VDO module and begin the initial scenario.
  my $device = $self->getDevice();
  my $scenario = $self->{_scenarios}{$self->{initialScenario}};

  $log->info("Setting up VDO $scenario->{version} on "
             . $scenario->{machine}->getName());
  $device->switchToScenario($scenario);
  $device->setup();
  $device->verifyModuleVersion();
}

#############################################################################
# @inherit
##
sub reserveHosts {
  my ($self) = assertNumArgs(1, @_);
  my $rsvper = $self->getRSVPer();

  # Create list of scenario names with duplicates removed.
  # Any repeated scenarios should use the same host throughout the test.
  my %testScenarios = map { $_, 1 } ($self->{initialScenario},
                                     @{$self->{intermediateScenarios}});
  my @scenarioNames = keys(%testScenarios);

  # Create hash of existing reserved hosts.
  my $reserved = {};
  for my $host (@{$self->{prereservedHosts}}) {
    my @classes = $rsvper->getHostOSArchClasses($host);
    if (scalar(@classes) > 0 ) {
      @classes = sort(@classes);
    } else {
      next;
    }

    my $key = join(",", @classes);
    if (defined($reserved->{$key})) {
      push(@{$reserved->{$key}}, $host);
    } else {
      $reserved->{$key} = [$host];
    }
  }

  # Assign or reserve the host needed for each scenario.
  foreach my $name (@scenarioNames) {
    my $scenarioInfo = $SUPPORTED_SCENARIOS->{$name};
    my @classes = sort($scenarioInfo->{rsvpOSClass}, $scenarioInfo->{arch});

    my $key = join(",", @classes);
    my $host = undef;
    if (defined($reserved->{$key}) && scalar(@{$reserved->{$key}}) > 0) {
      $host = shift(@{$reserved->{$key}});
    } else {
      my $classString = join(",", @classes, $self->{clientClass});
      ($host) = $self->reserveNumHosts(1, $classString, $self->{clientLabel});
    }

    $self->{_scenarios}->{$name}{hostname} = $host;

    if ($name eq $self->{initialScenario}) {
      $self->{defaultHost} = $host;
    }
  }
}

#############################################################################
# Generate an initial scenario hash from a scenario specification.
#
# @param scenarioName  The name of the scenario to generate a hash for
#
# @return The scenario hash
##
sub generateScenarioHash {
  my ($self, $scenarioName) = assertNumArgs(2, @_);
  my $version = $SUPPORTED_SCENARIOS->{$scenarioName}{moduleVersion};

  return { name => $scenarioName, version => $version };
}

#############################################################################
# Test basic read/write and dedupe capability before a migration.
##
sub setupDevice {
  my ($self) = assertNumArgs(1, @_);

  # Write an initial slice to the pass-through and verify.
  $self->{_testData} = [$self->createSlice(blockCount => $self->{blockCount})];
  $self->{_testData}[0]->write(tag => "initial", direct => 1, sync => 1);
  $self->{_testData}[0]->verify();
}

#############################################################################
# Test basic read/write and dedupe capability after a migration.
##
sub verifyScenario {
  my ($self) = assertNumArgs(1, @_);

  # Write a new slice and then verify all slices of data.
  my $index = scalar(@{$self->{_testData}});
  my $newSlice = $self->createSlice(blockCount => $self->{blockCount},
                                    offset => $self->{blockCount} * $index);
  $newSlice->write(tag => "S" . $index, direct => 1, sync => 1);
  $self->{_testData}[$index] = $newSlice;

  foreach my $slice (@{$self->{_testData}}) {
    $slice->verify();
  }
}

#############################################################################
# Verify the final state of the device.
##
sub verifyDevice {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  # Reboot then continue to use the VDO.
  $self->rebootMachineForDevice($device);
  $self->verifyScenario();

  # Return to the initial scenario and verify.
  my $initialHost =
    $self->{_scenarios}->{$self->{initialScenario}}{machine}->getName();
  $log->info("Returning to $self->{initialScenario} on $initialHost");
  $self->switchToIntermediateScenario($self->{initialScenario});
  $self->verifyScenario();
}

#############################################################################
# Move the VDO device to a new scenario.
##
sub switchToIntermediateScenario {
  my ($self, $scenarioName) = assertNumArgs(2, @_);
  my $scenario = $self->{_scenarios}{$scenarioName};
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
  my @scenarioList = @{$self->{intermediateScenarios}};
  my $scenarios = join(" to ", @scenarioList);
  my $testType = ($self->isa("VDOTest::UpgradeBase")) ? "upgrading"
                                                      : "migrating";
  $log->info("Test $testType $self->{initialScenario} to $scenarios");

  $self->setupDevice();
  my $device = $self->getDevice();
  foreach my $intermediateScenario (@scenarioList) {
    if ($device->needsExplicitUpgrade($intermediateScenario)) {
      $log->info(ucfirst($testType) . " to $intermediateScenario explicitly");
      $self->doUpgrade($intermediateScenario);
    } else {
      $log->info(ucfirst($testType) . " to $intermediateScenario implicitly");
      $self->switchToIntermediateScenario($intermediateScenario);
    }

    if (!$dontVerify) {
      $self->verifyScenario();
    }
  }

  if (!$dontVerify) {
    $self->verifyDevice();
  }
}

1;
