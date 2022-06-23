##
# The "checkin" vdo nightly build. This subset of tests is
# intended for testing changes to the nightly infrastructure.
#
# $Id$
##
package NightlyBuildType::VDOCheckin;

use strict;
use warnings FATAL => qw(all);

use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);

use NightlyRunRules;

use base qw(NightlyBuildType::VDO);

# Log4perl Logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $SUITE_PROPERTIES = {
  perlLocalCheckinTests => {
    displayName => "Perl_Local_Tests",
    suiteName   => "",
    type        => "perl",
  },
  udsLocalCheckinTests => {
    displayName => "UDS_Local_Checkin_Tests",
    suiteName   => "checkin",
    scale       => "FARM",
    type        => "uds",
  },
  vdoLocalCheckinTests => {
    displayName => "VDO_Local_Checkin_Tests",
    suiteName   => "checkin",
    scale       => "FARM",
  },
};

######################################################################
# Get the map of test suites to run.
##
sub getSuitesImplementation {
  my ($self) = assertNumArgs(1, @_);
  return generateTestSuites($SUITE_PROPERTIES, ["FEDORA35"]);
}

1;
