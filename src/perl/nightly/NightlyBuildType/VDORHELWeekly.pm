##
# The "standard" vdo weekly build, but on RHEL.
#
# $Id$
##
package NightlyBuildType::VDORHELWeekly;

use strict;
use warnings FATAL => qw(all);

use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertNumArgs
);

use NightlyRunRules;

use base qw(NightlyBuildType::VDOWeekly);

# Log4perl Logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

######################################################################
# Get the map of test suites to run.
##
sub getSuitesImplementation {
  my ($self) = assertNumArgs(1, @_);
  return generateTestSuites($self->getSuiteProperties(), ['RHEL10']);
}

1;
