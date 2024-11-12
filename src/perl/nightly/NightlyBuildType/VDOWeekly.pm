##
# The "standard" vdo weekly build.
#
# $Id$
##
package NightlyBuildType::VDOWeekly;

use strict;
use warnings FATAL => qw(all);

use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertNumArgs
);

use NightlyRunRules;

use base qw(NightlyBuildType::Single NightlyBuildType::VDOBuilder);

# Log4perl Logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

# The tests to run weekly
my $SUITE_PROPERTIES = {
  vdoUpgrade => {
    displayName => "VDO_Upgrade_Tests",
    suiteName   => "weeklyVDOUpgradeTests",
    scale       => "ALBIREO-PMI",
    extraArgs   => "--clientClass=ALBIREO-PMI",
  },
  weeklyVDOLocalTests => {
    displayName => "VDO_Local_Weekly_Tests",
    suiteName   => "weeklyVDOLocalTests",
    extraArgs   => "--threads=3",
  },
  weeklyVDOPerfTests => {
    displayName => "VDO_Weekly_Perf_Tests",
    suiteName   => "weeklyVDOPerfTests",
    extraArgs   => "--threads=2 --clientClass=VDO-PMI"
                   . " --logicalThreadCount=3"
                   . " --physicalThreadCount=2",
  },
  weeklyVDOSingleThreadPerfTests => {
    displayName => "VDO_Single_Thread_Perf_Tests",
    suiteName   => "weeklyVDOSingleThreadPerfTests",
    extraArgs   => "--threads=3 --clientClass=VDO-PMI",
  },
  weeklyVDOTests => {
    displayName => "VDO_Weekly_Tests",
    suiteName   => "weeklyVDOTests",
    extraArgs   => "--threads=3 --clientClass=PFARM",
  },
};

######################################################################
# Get the hash of distinct test suites to run.
##
sub getSuiteProperties {
  my ($self) = assertNumArgs(1, @_);
  return $SUITE_PROPERTIES;
}

######################################################################
# Get the map of test suites to run.
##
sub getSuitesImplementation {
  my ($self) = assertNumArgs(1, @_);
  return generateTestSuites($self->getSuiteProperties(), $DEFAULT_OS_CLASSES);
}

1;
