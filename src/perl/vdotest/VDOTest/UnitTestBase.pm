##
# Run C unit tests with VDO
#
# $Id$
##
package VDOTest::UnitTestBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Carp qw(croak);
use Permabit::Albtest;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Utils qw(makeFullPath);
use Permabit::Version qw(getDefaultTopdir);

use base qw(VDOTest);

our %PROPERTIES
  = (
     # @ple The glob patterns for selecting tests
     globPattern => undef,
     # @ple The directory in which to find the tests
     testDir     => "src/c++/vdo/tests",
    );

#############################################################################
# @inherit
##
sub listSharedFiles {
  my ($self) = assertNumArgs(1, @_);
  return (
          $self->SUPER::listSharedFiles(),
          "src/c++/vdo/tests/*.so",
	  "src/c++/vdo/tests/testdata",
          "src/c++/vdo/tests/vdotest",
         );
}

#############################################################################
# @inherit
##
sub suite {
  my ($package) = assertNumArgs(1, @_);

  # Get a dummy test to access test parameters and command line options.
  my $options = $package->makeDummyTest();
  my @testFiles = glob(makeFullPath(getDefaultTopdir(),
                                    $options->{testDir},
                                    $options->{globPattern}));

  my $suite = Test::Unit::TestSuite->empty_new($package);
  for my $testFile (grep { $package->testFilter($_) } @testFiles) {
    if ($testFile !~ m{^.*/(.+)$}) {
      croak("$testFile is a bad test file name");
    }
    my $testname = $1;
    my $testcase = $testname;
    if ($testname =~ m{^(\w+)\.\w+$}) {
      $testcase = $1;
    }
    my $fullName = $suite->name() . "::" . $testcase;
    my $sub = sub {
      my ($self) = assertNumArgs(1, @_);
      $self->runAlbtest($testname);
    };
    my $test = $package->make_test_from_coderef($sub, $fullName);
    $suite->add_test($test);
  }
  return $suite;
}

#############################################################################
# Filter that selects tests to be run
#
# @param name  Name of the test file
#
# @return true if the test should be run
##
sub testFilter {
  my ($self, $name) = assertNumArgs(2, @_);
  return 1;
}

#############################################################################
# Runs a C test on a remote machine.  The test can be a cunit test (a shared
# object) or an executable.
#
# @param testName   name of test to run
##
sub runAlbtest {
  my ($self, $testName) = assertNumArgs(2, @_);
  my $env = {
             ALBTEST_DIR => $self->{binaryDir},
            };
  my $binaryName = $testName;
  if ($testName =~ s/\.so$//) {
    $binaryName = 'vdotest';
  }
  my $args = {
              env      => $env,
              host     => $self->{clientNames}[0],
              name     => $binaryName,
              testName => $testName,
             };
  Permabit::Albtest->new($self, $args)->run();
}

1;
