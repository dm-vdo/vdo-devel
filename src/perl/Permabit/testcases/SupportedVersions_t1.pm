##
# Test the Permabit::SupportedVersions SUPPORTED_VERSIONS table and the
# associated utility functions.
#
# $Id$
##
package testcases::SupportedVersions_t1;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use List::Util qw(any);
use YAML;

use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertEqualNumeric
  assertFalse
  assertFileExists
  assertNotDefined
  assertNumArgs
  assertTrue
);
use Permabit::SupportedVersions qw(
  $SUPPORTED_ARCHITECTURES
  $SUPPORTED_OSES
  $SUPPORTED_SCENARIOS
  $SUPPORTED_VERSIONS
  compareVersionNumbers
  getDirectUpgradesToVersion
);
use Permabit::Version qw($VDO_MARKETING_VERSION);

use base qw(Permabit::Testcase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# Call compareVersionNumbers on two version strings and assert the return
# value is correct for both comparison orders.
#
# @param expected  The expected relation (-1, 0, 1)
# @param a         The first version string
# @param b         The second version string
##
sub _assertVersionOrder {
  my ($expected, $a, $b) = assertNumArgs(3, @_);
  assertEqualNumeric($expected, compareVersionNumbers($a, $b));
  assertEqualNumeric(-$expected, compareVersionNumbers($b, $a));
}

#############################################################################
# Exercise and sanity-check compareVersionNumbers.
##
sub testCompareVersionNumbers {
  _assertVersionOrder(0, "0.1", "0.1");
  _assertVersionOrder(1, "0.2", "0.1");
  _assertVersionOrder(1, "0.3", "0.3.3");
  _assertVersionOrder(1, "0.40", "0.4");
  _assertVersionOrder(1, "0.50", "0.50.0");

  my @moduleVersions = map { $_->{moduleVersion} } values(%{$SUPPORTED_VERSIONS});
  $log->debug(join(" ", "unsorted:", @moduleVersions));
  $log->debug(join(" ", "sorted:", sort compareVersionNumbers @moduleVersions));
}

#############################################################################
# Make sure the module name of "head" was properly expanded, and that the
# other fields are as expected, since it's slightly different than releases.
##
sub testHead {
  my ($self) = assertNumArgs(1, @_);

  my %head = %{$SUPPORTED_VERSIONS->{head}};
  $log->debug("Head is currently version "
              . $SUPPORTED_VERSIONS->{head}->{moduleVersion});

  assertEq($VDO_MARKETING_VERSION, $head{moduleVersion});
  assertTrue($head{isCurrent});
  assertEq("head", $head{branch});
  assertEq("Permabit::Statistics::VDO", $head{statistics});
  assertNotDefined($head{path});
}

#############################################################################
# Check that all the expected fields are defined, and have sensible values
# (except for "head", which is special and handled above).
##
sub testVersionSchema {
  my ($self) = assertNumArgs(1, @_);

  foreach my $key (sort(keys(%{$SUPPORTED_VERSIONS}))) {
    if ($key eq "head") {
      # head doesn't quite fit the pattern, and is checked above.
      next;
    }
    $log->debug("checking version $key");
    my %info = %{$SUPPORTED_VERSIONS->{$key}};

    if ($info{isCurrent}) {
      assertEq($key, $info{moduleVersion} . "-current");
    } else {
      assertEq($key, $info{moduleVersion});
    }

    # XXX maybe use P4 integration to check for the branch spec?
    assertDefined($info{branch});

    # Verify that the stats class module is loadable.
    my $class = $info{statistics};
    assertDefined($class, "$key must have a statistics class");
    eval("use $class;");
    assertFalse($EVAL_ERROR, "could not load $class for $key: $EVAL_ERROR");

    assertFileExists($info{path});
  }
}

#############################################################################
# Check that all the expected fields are defined, and have sensible values.
##
sub testScenarioSchema {
  my ($self) = assertNumArgs(1, @_);

  foreach my $key (sort(keys(%{$SUPPORTED_SCENARIOS}))) {
    $log->debug("Checking scenario $key");
    my %info = %{$SUPPORTED_SCENARIOS->{$key}};

    assertTrue(any { $info{rsvpOSClass} eq $_ } @{$SUPPORTED_OSES}, "scenario"
               . " RSVP OS class $info{rsvpOSClass} does not match a supported"
               . " OS.");
    assertTrue(any { $info{arch} eq $_ } @{$SUPPORTED_ARCHITECTURES}, "scenario"
               . " architecture $info{arch} is not a supported architecture.");
    assertDefined($SUPPORTED_VERSIONS->{$info{moduleVersion}}, "scenario module"
                  . " version $info{moduleVersion} must be defined in the"
                  . " SUPPORTED_VERSIONS table.");
  }
}

#############################################################################
# Exercise and sanity-check getDirectUpgradesToVersion().
##
sub testGetDirectUpgradesToVersion {
  my ($self) = assertNumArgs(1, @_);

  foreach my $key (sort(keys(%{$SUPPORTED_VERSIONS}))) {
    if (!$SUPPORTED_VERSIONS->{$key}{isCurrent}) {
      next;
    }
    my @upgrades = getDirectUpgradesToVersion($key);
    # There is no guarantee that because a current version exists that there
    # is a direct upgrade from a previous version.
    if (scalar(@upgrades) > 0) {
      $log->debug("'$key' directly from: " . join(' ', @upgrades));
    }
  }
}

1;
