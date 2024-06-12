##
# Base class for tests which run dmtest python tests for vdo.  Runs each test
# in its own testcase.
#
# $Id$
##
package DMTest::Separated;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::SystemUtils qw(assertSystem);
use Permabit::UserMachine;
use Permabit::Utils qw(makeFullPath);

use base qw(DMTest::Grouped);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
##
sub suite {
  my ($package) = assertNumArgs(1, @_);

  my @testNames = ();
  my $options = $package->makeDummyTest();

  eval {
    # Reserve a machine so we can look up the test names
    $options->{rsvpMsg} = "Making $package suite";
    $options->reserveHostGroup("client");
    $options->set_up();
    my $dmtestList = $options->listTests($options->{dmtestName});
    my $hashes = $options->treeToHashes($dmtestList);
    @testNames = $options->hashesToTests($hashes, "");
  };
  my $eval_error = $EVAL_ERROR;
  # Always clean up
  $options->tear_down();
  $options->getRSVPer()->closeRSVP($package);
  if ($eval_error) {
    die($eval_error);
  }
  # Now it is a simple matter of making and returning the suite
  my $suite = Test::Unit::TestSuite->empty_new($package);
  foreach my $testName ( @testNames ) {
    my $test
      = $package->make_test_from_coderef(\&DMTest::Grouped::testRunner,
                                         "${package}::$testName");
    $test->{dmtestName} = $testName;
    $suite->add_test($test);
  }
  return $suite;
}

1;
