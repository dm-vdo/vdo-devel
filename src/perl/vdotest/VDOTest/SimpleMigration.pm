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
   initialScenario       => "X86_FEDORA40_head",
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
    initialScenario       => "X86_FEDORA40_head",
    # @ple The intermediate versions to go through
    intermediateScenarios => ["X86_FEDORA40_head"],
  );
}

sub testSimpleMigration {
  my ($self) = assertNumArgs(1, @_);

  $self->_runTest();
}

#############################################################################
# Run the migration test from MigrationBase, specifying multiple scenarios
# that involve migration.
##
sub propertiesMultipleMigration {
  return (
    # @ple The scenario to start with
    initialScenario       => "X86_FEDORA40_head",
    # @ple The intermediate versions to go through
    intermediateScenarios => ["X86_FEDORA40_head", "X86_FEDORA40_head"],
  );
}

sub testMultipleMigration {
  my ($self) = assertNumArgs(1, @_);

  $self->_runTest();
}

1;
