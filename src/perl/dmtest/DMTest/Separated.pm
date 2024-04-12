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
use Permabit::Utils qw(makeFullPath);

use base qw(DMTest::Grouped);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
##
sub suite {
  my ($package) = assertNumArgs(1, @_);

  my $options = $package->makeDummyTest();
  my $testList;
  if (defined($options->{dmTestName})
      && ($options->{dmTestName} ne "")
      && ($options->{dmTestName} !~ /[\[\]{}*?]/)) {
    # If unitTestName names isn't a pattern but appears to name exactly one
    # test, just use it. If the name is bad, we'll find out soon enough.
    $testList = $options->{dmTestName};
  } else {
    # TODO: Get all tests from dmtest
  }

  # Now it is a simple matter of making and returning the suite
  my $suite = Test::Unit::TestSuite->empty_new($package);
  foreach my $testName (sort(split(/\s+/, $testList))) {
    my $test
      = $package->make_test_from_coderef(\&DMTest::Grouped::testRunner,
                                         "${package}::$testName");
    $test->{unitTestName} = $testName;
    $suite->add_test($test);
  }
  return $suite;
}

1;
