##
# Test the Permabit::KernelUtils utilities
#
# $Id$
##
package testcases::KernelUtils_t1;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use List::Util qw(max min);

use Permabit::Assertions qw(
  assertGTNumeric
  assertLTNumeric
  assertNear
  assertNumArgs
  assertTrue
);
use Permabit::Constants;
use Permabit::KernelUtils qw(
  getAddressableMemory
  removeKernelMemoryLimiting
  setupKernelMemoryLimiting
);
use Permabit::Utils qw(ceilMultiple sizeToText);

use Permabit::LabUtils qw(getTotalRAM isVirtualMachine);

use base qw(Permabit::Testcase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The RSVP class clients should be reserved from
     clientClass => undef,
     # @ple The names of the machines to be used
     clientNames => [],
     # @ple The number of clients that will be used
     numClients  => 1,
    );
##

#############################################################################
# Add tests to the suite
#
# @param suite  The suite to add tests to
# @param name   String to insert into the test name
# @param key    Name of option to set for these tests
# @param value  Value of option to set for these tests
##
sub _addTests {
  my ($package, $suite, $name, $key, $value) = assertNumArgs(5, @_);
  my %cases = (
               limit    => \&testLimit,
              );
  while (my ($prefix, $coderef) = each(%cases)) {
    my $test = $package->make_test_from_coderef($coderef,
                                                "${package}::$prefix$name");
    $test->{clientNames} = [];
    $test->{clientClass} = undef;
    $test->{numClients}  = 1;
    $test->{$key}        = $value;
    $suite->add_test($test);
  }
}

#############################################################################
# Return the suite with all the tests
##
sub suite {
  my ($package) = assertNumArgs(1, @_);
  my $suite = Test::Unit::TestSuite->empty_new($package);
  # Get a dummy test to access test parameters and command line options.
  my $options = $package->makeDummyTest();
  if (scalar(@{$options->{clientNames}}) > 0) {
    # The command line specifies which hosts to run the testcases on
    my $clientNames
      = $package->canonicalizeHostnames($options->{clientNames});
    foreach my $clientName (@$clientNames) {
      $package->_addTests($suite, ucfirst($clientName), "clientNames",
                          [$clientName]);
    }
  } elsif (defined($options->{clientClass})) {
    # The command line specifies which RSVP class to run the testcases on
    $package->_addTests($suite, $options->{clientClass}, "clientClass",
                        $options->{clientClass});
  } else {
    # Run the testcases on a selection of RSVP classes
    my @hardwareClasses = qw(FARM);
    foreach my $hardwareClass (@hardwareClasses) {
      $package->_addTests($suite, $hardwareClass,
                          "clientClass", $hardwareClass);
    }
  }
  return $suite;
}

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();
  $self->reserveHostGroup("client");
}

######################################################################
# Verify that memory is limited as desired.
#
# @param hosts     A reference to the list of hosts to limit
# @param minimum   The minimum amount of memory to accept
# @param maximum   The maximum amount of memory to accept
# @param expected  The actual memory size expected
##
sub _verifyMemoryLimit {
  my ($hosts, $minimum, $maximum, $expected) = assertNumArgs(4, @_);
  $log->info("Testing memory range " . sizeToText($minimum)
             . " to " . sizeToText($maximum));
  setupKernelMemoryLimiting($hosts, $minimum, ($maximum - $minimum));
  my %hostLimitedMemory = map { $_ => getTotalRAM($_) } @{$hosts};
  foreach my $host (@{$hosts}) {
    assertNear($expected, $hostLimitedMemory{$host}, "2%",
               "$host memory was not limited appropriately:"
               . " got " . sizeToText($hostLimitedMemory{$host})
               . " but expected " . sizeToText($expected));
  }
  removeKernelMemoryLimiting($hosts);
}

######################################################################
##
sub testLimit {
  my ($self) = assertNumArgs(1, @_);
  my @hosts = (@{$self->{clientNames}});

  # Test that we cannot limit kernel memory to a ridiculously large value.
  eval {
    setupKernelMemoryLimiting(\@hosts, 10 * $TB, 0);
  };
  assertTrue($EVAL_ERROR);

  # Figure out the range of reasonable values for these hosts so we construct
  # interesting testcases.
  my %hostTotalMemory = map { $_ => getTotalRAM($_) } @hosts;
  my $minimumUnlimitedMemory = min(values(%hostTotalMemory));
  my $smallestMemoryAmount = max($GB, $minimumUnlimitedMemory / 3);
  foreach my $host (@hosts) {
    assertLTNumeric(($smallestMemoryAmount * 1.1), $hostTotalMemory{$host},
                    "$host does not have enough memory for a valid test");
  }

  # Test that too high an upper bound will simply use what is available.
  _verifyMemoryLimit(\@hosts, $smallestMemoryAmount,
                     $minimumUnlimitedMemory * 2, $minimumUnlimitedMemory);

  # Test various parts of the valid range. Figure out the increment between
  # successive tests, so we do not run too many iterations (for time reasons).
  # Make sure the increment is at least 0.5G, since testing values that are
  # too close together isn't very interesting.
  my $testRange = $minimumUnlimitedMemory - $smallestMemoryAmount;
  my $testIncrement = max(0.5 * $GB, ceilMultiple(0.2 * $testRange, $MB));

  my $testTarget = $smallestMemoryAmount;
  while ($testTarget < $minimumUnlimitedMemory) {
    $log->info("Testing " . sizeToText($testTarget));
    my $nextTarget = $testTarget + $testIncrement;
    _verifyMemoryLimit(\@hosts, $testTarget, $testTarget, $testTarget);
    _verifyMemoryLimit(\@hosts, $testTarget, $nextTarget,
                       min($nextTarget, $minimumUnlimitedMemory));
    $testTarget = $nextTarget;
  }
}

1;
