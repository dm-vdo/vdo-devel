##
# Base class for tests which run user mode linux unit tests.  Runs each test in
# its own testcase.
#
# $Id$
##
package UDSTest::UserSeparated;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::SystemUtils qw(assertSystem);
use Permabit::Utils qw(makeFullPath);

use base qw(UDSTest::UserGrouped);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

our @CHECKIN_TESTS;
our @FULL_TESTS;
our @JENKINS_TESTS;
our @PERF_TESTS;

our %PROPERTIES
  = (
     # @ple The test suite.  Use "checkin" or "jenkins" or "full" or
     #      "performance"
     unitTests => "checkin",
    );

#############################################################################
##
sub suite {
  my ($package) = assertNumArgs(1, @_);

  # Get a dummy test to access test parameters and command line options
  my $options = $package->makeDummyTest();
  my $udsDir = "src/c++/uds";
  my $testDir = makeFullPath($options->{topDir}, $udsDir,
			     $options->{platformDir}, "tests");

  # Read the lists of unit tests.
  my $result = assertSystem("make -s -C $testDir list-tests");
  eval($result->{stdout});
  if ($EVAL_ERROR) {
    die($EVAL_ERROR);
  }
  my %unitTestSuites = (
                        checkin     => \@CHECKIN_TESTS,
                        full        => \@FULL_TESTS,
                        jenkins     => \@JENKINS_TESTS,
                        performance => \@PERF_TESTS,
                       );

  my $suite = Test::Unit::TestSuite->empty_new($package);
  foreach my $testName (sort(@{$unitTestSuites{$options->{unitTests}}})) {
    my $test
      = $package->make_test_from_coderef(\&UDSTest::UserGrouped::testRunner,
                                         "${package}::$testName");
    $test->{unitTestName} = $testName;
    $suite->add_test($test);
  }
  return $suite;
}

1;
