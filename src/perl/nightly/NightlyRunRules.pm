#######################################################################
# Test execution routines used by nightly.pl
#
# $Id$
##
package NightlyRunRules;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

# The testing framework may use a string with a function name to be run
# so we need to make sure it is allowed.
no strict "refs";

use Log::Log4perl;
use NightlyUtils;
use Permabit::Assertions qw(
  assertMinMaxArgs
  assertNumArgs
);

use base qw(Exporter);

# By default, each test suites will be run on each listed OS class.
our $DEFAULT_OS_CLASSES = ["FEDORA41"];

our @EXPORT = qw(
  $DEFAULT_OS_CLASSES
  generateTestSuites
);

our $VERSION = 1.0;

# Log4perl Logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

######################################################################
# Create a subroutine for running a test from a hash of properties
#
# @param testProperties  A hashref of test suite properties
# @param osClass         The RSVP OS class to use
#
# @return A subroutines to run the test suite described
##
sub _generateTestSubroutine {
  my ($testProperties, $osClass) = assertNumArgs(2, @_);

  my $displayName = $testProperties->{displayName} . "_" . $osClass;
  my $type = $testProperties->{type} //= "vdo";
  if ($type eq "unit") {
    return sub {
      my ($properties) = assertNumArgs(1, @_);
      _runUnitTests($properties, $displayName, $osClass,
                    $testProperties->{suiteName});
    };
  }

  my $extraArgs = $testProperties->{extraArgs} // "";
  if (defined($testProperties->{scale})) {
    $extraArgs .= " --scale=$testProperties->{scale},$osClass";
  }

  return sub {
    my ($properties) = assertNumArgs(1, @_);
    _runTestSuite($properties, $displayName, $type, $osClass,
                  $testProperties->{suiteName}, $extraArgs);
  };
}

######################################################################
# Convert a set of test properties into a hash of tests to be run
#
# @param testMap      A hashref of test suite properties hashes
# @param osClassList  A listref of RSVP OS classes to use if not otherwise
#		      specified by a suite
#
# @return A hashref of subroutines to be run
##
sub generateTestSuites {
  my ($testMap, $osClassList) = assertNumArgs(2, @_);

  my $testsToRun = {};
  for my $suiteName (keys(%{$testMap})) {
    my $suiteInfo = $testMap->{$suiteName};
    my $osClasses = $suiteInfo->{osClasses} // $osClassList;
    for my $osClass (@{$osClasses}) {
      if (defined($suiteInfo->{excludedOSes})
	  && scalar(grep(/^$osClass$/, @{$suiteInfo->{excludedOSes}}))) {
	$log->debug("Skipping generation of $suiteName on $osClass");
	next;
      }
      my $testSub = _generateTestSubroutine($suiteInfo, $osClass);
      my $newKey = "$suiteName-$osClass";
      $testsToRun->{$newKey} = $testSub;
    }
  }

  return $testsToRun;
}

######################################################################
# Run the perl unit tests
#
# @param  properties  A hashref of runtime properties
# @param  testName    The name of the test suite
# @param  osClass     The default OS class to reserve machines from
# @oparam suite       The suite to run
##
sub _runUnitTests {
  my ($properties, $testName, $osClass, $suite)
    = assertMinMaxArgs([""], 3, 4, @_);

  my $logDir = "$properties->{runLogDir}/$testName/";
  my $cmd = "./runtests.pl --log=1 --JSON=1 --logDir=$logDir"
            . " --saveServerLogDir=$logDir --xmlOutput=1 --scale"
            . " --moveToMaintenance --rsvpOSClass=$osClass $suite";
  my $rundir = "$properties->{perlRoot}/Permabit";
  my $regexp = "The following tests failed";

  if ($properties->{skipTestExecution}) {
      $log->debug("Skipping execution of $testName with command: $cmd");
      return;
  }
  NightlyUtils::runTests($testName, $cmd, $rundir, $logDir, $regexp);
  installFiles("$logDir/*.xml", "$properties->{xmlLogDir}/perltests");
}

######################################################################
# Run the non-unit perl tests. If no suite is provided, use the default
# (nightly) test suite.
#
# @param  properties  A hashref of runtime properties
# @param  testName    A descriptive name for the tests being run
# @param  type        The test type (vdo, uds, or dm)
# @param  osClass     The default OS class to reserve machines from
# @oparam suite       The test suite to run
# @oparam otherArgs   Other arguments to pass to the tests
##
sub _runTestSuite {
  my ($properties, $testName, $type, $osClass, $suite, $otherArgs)
    = assertMinMaxArgs(['', ''], 4, 6, @_);

  my $logDir = "$properties->{runLogDir}/$testName/";
  my $rundir = "$properties->{perlRoot}/${type}test";
  my $regexp = "The following tests failed";

  my $cmd = "./${type}tests.pl $suite --log=1 --JSON=1"
            . " --logDir=$logDir"
            . " --xmlOutput=1"
            . " --saveServerLogDir=$logDir"
            . " --nightlyStart=$properties->{nightlyStart}"
            . " --moveToMaintenance --rsvpOSClass=$osClass $otherArgs";

  if ($properties->{skipTestExecution}) {
    $log->debug("Skipping execution of $testName with command: $cmd");
    return;
  }
  NightlyUtils::runTests($testName, $cmd, $rundir, $logDir, $regexp);
  installFiles("$logDir/*.xml", "$properties->{xmlLogDir}/${type}test");
}

1;
