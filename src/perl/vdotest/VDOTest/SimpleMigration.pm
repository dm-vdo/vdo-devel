##
# Verify that VDO can be migrated to an alternate machine.
#
# $Id$
##
package VDOTest::SimpleMigration;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertNumArgs
);
use Permabit::Constants;
use Permabit::Utils qw(makeFullPath);

use base qw(VDOTest::MigrationBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple The scenario to start with
   initialScenario       => "X86_RHEL9_8.2.1-current",
   # @ple The intermediate scenarios to go through
   intermediateScenarios => [],
   # @ple VDO physical size
   physicalSize          => 20 * $GB,
  );
##

#############################################################################
# Run the basic migration test from MigrationBase.
##
sub propertiesSimpleMigration {
  return (
    # @ple The scenario to start with
    initialScenario       => "X86_RHEL9_8.2.1-current",
    # @ple The intermediate versions to go through
    intermediateScenarios => ["S390X_RHEL9_8.2.1-current"],
  );
}

sub testSimpleMigration {
  my ($self) = assertNumArgs(1, @_);

  $self->_runTest();
}

#############################################################################
# Run the migration test from MigrationBase, specifying scenarios that
# involve migration and upgrade.
##
sub propertiesMultipleMigration {
  return (
    # @ple The scenario to start with
    initialScenario       => "X86_RHEL9_8.2.1-current",
    # @ple The intermediate versions to go through
    intermediateScenarios => ["S390X_RHEL9_8.2.1-current", "AARCH64_RHEL9_8.2.1-current"],
  );
}

sub testMultipleMigration {
  my ($self) = assertNumArgs(1, @_);

  $self->_runTest();
}

#############################################################################
# Run the migration test from MigrationBase, specifying scenarios that
# involve migration and upgrade.
##
sub propertiesMigrateAndUpgrade {
  return (
    # @ple The scenario to start with
    initialScenario       => "X86_RHEL9_8.2.1.3",
    # @ple The intermediate versions to go through
    intermediateScenarios => ["X86_RHEL9_8.2.1-current"],
  );
}

sub testMigrateAndUpgrade {
  my ($self) = assertNumArgs(1, @_);

  $self->_runTest();
}

1;
