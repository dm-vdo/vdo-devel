######################################################################
# A package that loads the tables of supported VDO versions and migration
# scenarios and exports them as SUPPORTED_VERSIONS and SUPPORTED_SCENARIOS
# hashes, respectively.
#
# The SUPPORTED_VERSIONS hash provides the applicable release versions, branch
# mappings, statistics class names, and release directories.
#
# The SUPPORTED_SCENARIOS hash provides the applicable RSVP machine OS classes,
# host architectures, and kernel module versions.
#
# @synopsis
#
#   use Permabit::SupportedVersions;
#   $log->debug("Head is currently version "
#               . $SUPPORTED_VERSIONS->{head}->{moduleVersion});
#   $log->debug("The head scenario supports architecture "
#               . $SUPPORTED_SCENARIOS->{head}->{arch});
#
# @description
#
# C<Permabit::SupportedVersions> exports two hashrefs: $SUPPORTED_VERSIONS and
# $SUPPORTED_SCENARIOS. The hashrefs contain entries from the
# SupportedVersions.yaml and SupportedScenarios.yaml files, respectively.
# They are exported in the same directory as this module, in the following
# forms:
#
#   "8.2.1.3" => {
#     moduleVersion => "8.2.1.3",
#     isCurrent     => 0,
#     branch        => "chlorine",
#     statistics    => "Permabit::Statistics::VDO",
#     path          => "/permabit/release/source/vdo-chlorine/"
#                      . "vdo-chlorine-8-2-1-3",
#   }
#
#   "X86_RHEL9_8.2.1.3" => {
#     rsvpOSClass   => "RHEL9",
#     arch          => "X86_64",
#     moduleVersion => "8.2.1.3",
#   }
#
# $Id$
##
package Permabit::SupportedVersions;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use File::Basename qw(dirname);
use File::Spec;
use List::Util qw(min);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertTrue
  assertNumArgs
);
use Permabit::Utils qw(getYamlHash);
use Permabit::Version;

use Data::Dumper;

use base qw(Exporter);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

our @EXPORT_OK = qw(
  $SUPPORTED_ARCHITECTURES
  $SUPPORTED_OSES
  $SUPPORTED_SCENARIOS
  $SUPPORTED_VERSIONS
  compareVersionNumbers
  getDirectUpgradesToVersion
);

our $SUPPORTED_OSES = ["RHEL8", "RHEL9", "FEDORA40", "FEDORA41"];
our $SUPPORTED_ARCHITECTURES = ["X86_64", "AARCH64", "PPC64LE", "S390X"];

# The name of the versions and scenarios file to load, which is assumed to
# exist in the same directory as this module.
our $SUPPORTED_VERSIONS_FILE = "SupportedVersions.yaml";
our $SUPPORTED_SCENARIOS_FILE = "SupportedScenarios.yaml";

# The hashrefs exported by the module, containing the tables of version and
# scenario info.
our $SUPPORTED_VERSIONS = _loadSupportedVersions();
our $SUPPORTED_SCENARIOS = _loadSupportedScenarios();

#############################################################################
# Perl comparator function for version number strings of the form "6.2" and
# "10.19.2.111". Specified elements are defined to preceded unspecified ones,
# so "1.2.3.4" precedes "1.2" since "1.2" is always the latest version in the
# "1.2.*.*" version series.
##
sub compareVersionNumbers ($$) { ## no critic (ProhibitSubroutinePrototypes)
  my @aParts = split(/\./, $_[0]);
  my @bParts = split(/\./, $_[1]);

  # Return the relative order of the first non-equal elements of the array.
  my $minLength = min(scalar(@aParts), scalar(@bParts));
  for (my $i = 0; $i < $minLength; $i++) {
    my $relation = $aParts[$i] <=> $bParts[$i];
    if ($relation != 0) {
      return $relation;
    }
  }

  # One is a prefix (or equal to) the other; the shorter one is greater.
  return scalar(@bParts) <=> scalar(@aParts);
}

#############################################################################
# Get the version number for a specified version key.
#
# @param key  The key in the SUPPORTED_VERSIONS hash
#
# @return the moduleVersion property of the specified version
##
sub _getVersionNumber {
  my ($key) = assertNumArgs(1, @_);
  return $SUPPORTED_VERSIONS->{$key}{moduleVersion};
}

#############################################################################
# Perl comparator function for version keys that uses the order of the version
# numbers associated with the keys.
##
sub _compareVersionKeys ($$) { ## no critic (ProhibitSubroutinePrototypes)
  compareVersionNumbers(_getVersionNumber($_[0]), _getVersionNumber($_[1]))
}

#############################################################################
# Get an array of version keys in $SUPPORTED_VERSIONS that have version
# numbers matching the version number prefix from a specified version. For
# example, "5.2-current" matches "5.2-current" and "5.2.0.74", but not
# "5.20.1.1". The matching is based on the moduleVersion property, so "main"
# is also valid input.
#
# @param key  The version key specifying the version prefix to match
#
# @return an unsorted array of matching version keys
##
sub _getMatchingVersionKeys {
  my ($key) = assertNumArgs(1, @_);
  my $prefix = _getVersionNumber($key);
  return grep { /^\Q$prefix\E($|[.-])/ } keys(%{$SUPPORTED_VERSIONS});
}

#############################################################################
# Return a list of all versions that can be directly upgraded to head or to
# the current version on a branch. For head, this is likely to be all versions
# the latest release. For a release branch, it should be all upgradable
# versions of the previous release and all released versions of the branch.
#
# @param target The version key of the target version of the upgrade
#               ("head", "5.2-current", etc)
#
# @return A sorted list of version keys directly upgradable to the target
##
sub getDirectUpgradesToVersion {
  my ($target) = assertNumArgs(1, @_);
  assertDefined($SUPPORTED_VERSIONS->{$target},
                "'$target' must be a known supported version");
  assertTrue($SUPPORTED_VERSIONS->{$target}{isCurrent},
             "'$target' must be a current version");

  # Get the version keys of just the current versions with numbers less than
  # the target version number, in sorted order.
  my @currentKeys = grep {
    $SUPPORTED_VERSIONS->{$_}{isCurrent}
      && (_compareVersionKeys($_, $target) < 0)
  } sort { _compareVersionKeys($a, $b) } keys(%{$SUPPORTED_VERSIONS});

  # The greatest version preceding the target version is the last in the array.
  my $predecessor = pop(@currentKeys);

  # Get the keys of versions matching the target, excluding the target itself.
  my @upgrades = grep { $_ ne $target } _getMatchingVersionKeys($target);

  if (defined($predecessor)) {
    # Add all the versions from the previous release. These may not always be
    # directly upgradable to the target, but for now we assume they are.
    push(@upgrades, _getMatchingVersionKeys($predecessor));
  }

  # Return the list of keys in ascending version number order.
  return sort { _compareVersionKeys($a, $b) } @upgrades;
}

#############################################################################
# For each member of a hash containing hashes or scalars, replace each scalar
# with the output of a provided function on that scalar, recursively handling
# scalars at any depth in the hash.
##
sub _traverseHash {
  my ($hash, $function) = assertNumArgs(2, @_);
  foreach my $k (keys(%{$hash})) {
    if (ref($hash->{$k}) eq 'HASH') {
       _traverseHash($hash->{$k}, $function);
    } else {
      $hash->{$k} = $function->($hash->{$k});
    }
  }
}

#############################################################################
# Load the YAML file containing the table of supported VDO release versions
# and return it as a hashref.
#
# @return a hash containing supported VDO versions
##
sub _loadSupportedVersions {
  # Load the YAML file from the same directory as this module.
  my $path = File::Spec->catfile(dirname(__FILE__), $SUPPORTED_VERSIONS_FILE);
  my $versions = getYamlHash($path);

  # Expand the symbol from CURRENT_VERSION referenced by head.moduleVersion,
  # if it has a definition exported by Permabit::Version.
  my $replaceVersionNumbers = sub {
    my ($val) = assertNumArgs(1, @_);
    if (!defined($val)) {
      return $val;
    }
    my $symbol = $Permabit::Version::{$val};
    if (defined($symbol)) {
      return ${$symbol};
    }
    return $val;
  };

  _traverseHash($versions, $replaceVersionNumbers);
  return $versions;
}

#############################################################################
# Load the YAML file containing the table of supported VDO scenarios for
# testing and return it as a hashref.
#
# @return a hash containing supported VDO scenarios
##
sub _loadSupportedScenarios {
  # Load the YAML file from the same directory as this module.
  my $path = File::Spec->catfile(dirname(__FILE__), $SUPPORTED_SCENARIOS_FILE);
  my $scenarios = getYamlHash($path);

  return $scenarios;
}

1;
