##
# Verify that VDO can be upgraded from the last branch across all supported
# architectures.
#
# $Id$
##
package VDOTest::MultiArchUpgrade;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertNumArgs
);
use Permabit::Constants;
use Permabit::SupportedVersions qw($SUPPORTED_SCENARIOS $SUPPORTED_VERSIONS);

use base qw(VDOTest::UpgradeBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple VDO physical size
   physicalSize => 20 * $GB,
   # @ple VDO slab bit count
   slabBits     => $SLAB_BITS_TINY,
  );
##

my $TEST_SCENARIOS  = ["X86_RHEL9_head",
                       "AARCH64_RHEL9_head",
                       # Temporarily removing ppc64le, as Beaker installations
                       # of this arch are unstable.
                       # "PPC64LE_RHEL9_head",
                       #"S390X_RHEL9_head"
                      ];

#############################################################################
# Return the suite with all the tests.
##
sub suite {
  my ($package) = assertNumArgs(1, @_);

  # Fail immediately if any of the scenarios or versions are unknown.
  foreach my $scenario (@{$TEST_SCENARIOS}) {
    assertDefined($SUPPORTED_SCENARIOS->{$scenario},
                  "'$scenario' must be in SUPPORTED_SCENARIOS to test it.");

    my $version = $SUPPORTED_SCENARIOS->{$scenario}{moduleVersion};
    assertDefined($SUPPORTED_VERSIONS->{$version},
                  "'$version' must be in SUPPORTED_VERSIONS to test it");
  }

  # Generate the test name here, and store it in a hash so we remove
  # duplicate testcases.
  my %tests;
  my $name = "test" . join("_To_", @{$TEST_SCENARIOS}, "X86_head");

  # Substitute for characters not allowed in test names.
  $name =~ tr/.-/_/;
  $tests{$name} = $TEST_SCENARIOS;

  my $suite = Test::Unit::TestSuite->empty_new($package);
  foreach my $testName (keys(%tests)) {
    $suite->add_test($package->makeTest($testName, $tests{$testName}));
  }

  return $suite;
}

1;
