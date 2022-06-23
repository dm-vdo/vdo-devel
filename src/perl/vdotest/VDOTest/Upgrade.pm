##
# Verify that VDO can be upgraded.
#
# As an overarching theme, upgrades within a branch should implicitly work,
# with no extra upgrade step needed; and upgrades from the previous branch
# should work. Currently, we do not support any upgrades to Magnesium from
# earlier versions.
#
# $Id$
##
package VDOTest::Upgrade;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertNumArgs
);
use Permabit::Constants;
use Permabit::SupportedVersions qw($SUPPORTED_VERSIONS);

use base qw(VDOTest::UpgradeBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple VDO physical size
   physicalSize => 50 * $GB,
   # @ple VDO slab bit count
   slabBits     => $SLAB_BITS_TINY,
  );
##

# Released versions that support upgrades.
my $SULFUR_VERSIONS = ["8.1.0-current"];

# A list of regexes for upgrades from released versions that do not work.
my @BAD_UPGRADES = ();

#############################################################################
# Generate all non-empty ordered subsets of a given list.
#
# @param referenceList  The arrayref to generate subsets of
#
# @return An arrayref of ref to all non-empty subsets.
##
sub nonEmptySubsets {
  my ($referenceList) = assertNumArgs(1, @_);
  my @subsetList = ([]);
  foreach my $element (@$referenceList) {
    @subsetList = map { ($_ , [ @$_, $element ]) } @subsetList;
  }
  # Remove the empty subset, guaranteed by construction to be first.
  shift(@subsetList);
  return \@subsetList;
}

#############################################################################
# Generate all lists containing one element from each given list.
# Takes a list of arrayrefs of arrayrefs.
##
sub allCombinations {
  my @choiceLists = @_;
  my $permutationList = [[]];
  foreach my $choiceList (@choiceLists) {
    my $oldPermutationList = $permutationList;
    $permutationList = [];
    foreach my $choice (@$choiceList) {
      push(@$permutationList, map { [ @$_, @$choice ] } @$oldPermutationList);
    }
  }
  return $permutationList;
}

#############################################################################
# Generate all possible upgrade paths using a set of builds.
#
# @return An arrayref of arrayrefs of upgrade paths.
##
sub generateUpgradePathsFromVersions {
  my ($aluminumVersions) = assertNumArgs(1, @_);
  my $aluminumPaths = nonEmptySubsets($aluminumVersions);

  # We must upgrade to every branch after the one we start with, so we cannot
  # take all subsets of all paths, but must generate paths for each branch
  # separately.
  my $aluminumStartingPaths = $aluminumPaths;

  return [
          @$aluminumStartingPaths,
         ];
}

#############################################################################
# Generate tests from current to current.
##
sub generateHeadPaths {
  return [ ["head"] ];
}

#############################################################################
# Return the suite with all the tests.
##
sub suite {
  my ($package) = assertNumArgs(1, @_);

  my $upgradePaths = generateUpgradePathsFromVersions($SULFUR_VERSIONS);
  my $headPaths = generateHeadPaths();
  my @allPaths = (@$headPaths, @$upgradePaths);

  # Generate the test names here, and store them in a hash so we remove
  # duplicate testcases.
  my %tests;
  foreach my $versionList (@allPaths) {
    # Fail immediately if any of the versions are unknown.
    foreach my $version (@$versionList) {
      assertDefined($SUPPORTED_VERSIONS->{$version},
                    "'$version' must be in SUPPORTED_VERSIONS to test it");
    }

    my $name = "test" . join("_To_", @$versionList, "Head");
    # Substitute for characters not allowed in test names.
    $name =~ tr/.-/_/;
    $tests{$name} = $versionList;
  }

  my $suite = Test::Unit::TestSuite->empty_new($package);
  foreach my $testName (keys(%tests)) {
    if (scalar(grep { $testName =~ $_} @BAD_UPGRADES) > 0) {
      next;
    }

    $suite->add_test($package->makeTest($testName, $tests{$testName}));
  }

  return $suite;
}

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();
}

1;
