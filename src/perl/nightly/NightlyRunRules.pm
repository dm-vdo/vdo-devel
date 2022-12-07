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
our $DEFAULT_OS_CLASSES = ["FEDORA36", "FEDORA37"];

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
  if ($type eq "perl") {
    return sub {
      my ($properties) = assertNumArgs(1, @_);
      _runPerlTests($properties, $displayName, $osClass,
                    $testProperties->{suiteName});
    };
  }

  my $extraArgs = $testProperties->{extraArgs} // "";
  if (defined($testProperties->{scale})) {
    $extraArgs .= " --scale=$testProperties->{scale},$osClass";
  }

  if ($type eq "uds") {
    return sub {
      my ($properties) = assertNumArgs(1, @_);
      _runUDSTestSuite($properties, $displayName, $osClass,
                       $testProperties->{suiteName}, $extraArgs);
    };
  }

  return sub {
    my ($properties) = assertNumArgs(1, @_);
    _runVDOTestSuite($properties, $displayName, $osClass,
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
sub _runPerlTests {
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
# Run the perl VDO tests. The targets are sets of tests defined in
# vdotests.suites. The default target is the default (nightly) test suite.
#
# @param  properties  A hashref of runtime properties
# @param  testName    A descriptive name for the tests being run
# @param  osClass     The default OS class to reserve machines from
# @oparam suite       The VDO test suite to run
# @oparam otherArgs   Other arguments to pass to the tests
##
sub _runVDOTestSuite {
  my ($properties, $testName, $osClass, $suite, $otherArgs)
    = assertMinMaxArgs(['', ''], 3, 5, @_);

  my $logDir = "$properties->{runLogDir}/$testName/";
  my $rundir = "$properties->{perlRoot}/vdotest";
  my $regexp = "The following tests failed";

  my $cmd = "./vdotests.pl $suite --log=1 --JSON=1"
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
  installFiles("$logDir/*.xml", "$properties->{xmlLogDir}/vdotest");
}

######################################################################
# Run the perl UDS tests. The targets are sets of tests defined in
# udstests.suites. The default target is the default test suite.
#
# @param  properties  A hashref of runtime properties
# @param  testName    A descriptive name for the tests being run
# @param  osClass     The default OS class to reserve machines from
# @oparam suite       The UDS test suite to run
# @oparam otherArgs   Other arguments to pass to the tests
##
sub _runUDSTestSuite {
  my ($properties, $testName, $osClass, $suite, $otherArgs)
    = assertMinMaxArgs(['', ''], 3, 5, @_);

  my $logDir = "$properties->{runLogDir}/$testName/";
  my $rundir = "$properties->{perlRoot}/udstest";
  my $regexp = "The following tests failed";

  my $cmd = "./udstests.pl $suite --log=1 --JSON=1"
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
  installFiles("$logDir/*.xml", "$properties->{xmlLogDir}/udstest");
}

1;
