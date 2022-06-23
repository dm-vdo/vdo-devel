##
# Base class for tests which run kernel mode linux unit tests.  Runs each test
# in its own testcase.
#
# $Id$
##
package UDSTest::KernelSeparated;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::PlatformUtils qw(isSqueeze);
use Permabit::SystemUtils qw(assertSystem);
use Permabit::Utils qw(makeFullPath);

use base qw(UDSTest::KernelGrouped);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
##
sub suite {
  my ($package) = assertNumArgs(1, @_);

  my $options = $package->makeDummyTest();
  my $testList;
  if (defined($options->{unitTestName})
      && ($options->{unitTestName} ne "")
      && ($options->{unitTestName} !~ /[\[\]{}*?]/)) {
    # If unitTestName names isn't a pattern but appears to name exactly one
    # test, just use it. If the name is bad, we'll find out soon enough.
    $testList = $options->{unitTestName};
  } else {
    # Get a dummy test to access test parameters and command line options and
    # test infrastructure
    my ($machine, $module);
    eval {
      # Reserve a machine so we can look up the test names
      $options->{rsvpMsg} = "Making $package suite";
      $options->reserveHostGroup("client");
      $machine = $options->getUserMachine();
      # Load the test module
      my $udsDir = "src/c++/uds";
      my $modDir = makeFullPath($options->{topDir}, $udsDir,
				"kernelLinux", "tests");
      my $initrd = isSqueeze($machine->getName()) ? 1 : 0;
      $module = Permabit::KernelModule->new(initrd     => $initrd,
					    machine    => $machine,
					    modDir     => $modDir,
					    modName    => "zubenelgenubi",
					    modVersion => 1,);
      $module->load();
      # Now fetch the test names;
      my $moduleDir = "/sys/zubenelgenubi";
      $machine->runSystemCmd("(cd $moduleDir; ls -d $options->{unitTestName})");
      $testList = $machine->getStdout();
    };
    # Always clean up the machine
    my $eval_error = $EVAL_ERROR;
    if (defined($module)) {
      eval { $module->unload(); };
    }
    if (defined($machine)) {
      $machine->close();
    }
    $options->getRSVPer()->closeRSVP($package);
    if ($eval_error) {
      die($eval_error);
    }
  }

  # Now it is a simple matter of making and returning the suite
  my $suite = Test::Unit::TestSuite->empty_new($package);
  foreach my $testName (sort(split(/\s+/, $testList))) {
    my $test
      = $package->make_test_from_coderef(\&UDSTest::KernelGrouped::testRunner,
                                         "${package}::$testName");
    $test->{unitTestName} = $testName;
    $suite->add_test($test);
  }
  return $suite;
}

1;
