##
# Test VDO with blocks that compress really well.
#
# The point of this test is that the amount of compression has no more
# than 10% variation.
#
# The third point of this test is that MurmurHash3 collisions does not
# affect the amount of compression.
#
# $Id$
##
package VDOTest::Compress02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use List::Util qw(max min sum);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertEqualNumeric
  assertNear
  assertNumArgs
);
use Permabit::Utils qw(makeFullPath sizeToText timeToText);
use Time::HiRes qw(sleep time);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my @SAME_STATS = (
                  "dedupe matches (derived)",
                  "logical blocks",
                  "overhead blocks used",
                  "physical blocks",
                 );

my @RANGE_STATS = (
                   "compressed blocks written",
                   "compressed fragments written",
                   "data blocks used",
                   "dedupe mismatches (derived)",
                  );

my @ALL_STATS = sort(@SAME_STATS, @RANGE_STATS);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks in a dataset
     blockCount        => 200000,
     # @ple if defined, set up this type of device for the test
     deviceType        => "lvmvdo",
     # @ple Enable compression on the VDO device
     enableCompression => 1,
    );
##

#############################################################################
# Return the suite with all the tests
##
sub suite {
  my ($package) = assertNumArgs(1, @_);

  # Get a dummy test to access test parameters and command line options.
  my $options = $package->makeDummyTest();

  # Run the test ten times and examine the range of compression results.
  my @normalNames = (0..9);
  my @normalResults;
  my @tests;
  for my $name (@normalNames) {
    my $testname = "${package}::testCompress" . ucfirst($name);
    my $test = $package->make_test_from_coderef(\&_testCompress, $testname);
    my $results = { testname => $testname };
    $test->{_name}      = $name;
    $test->{_results}   = $results;
    $test->{_type}      = "normal";
    push(@tests, $test);
    push(@normalResults, $results);
  }

  # Due to asynchronous packing, we don't expect results to reproduce exactly.
  my $name = "${package}::checkResultsWithoutCollisions";
  my $checkSub = \&_checkResults;
  my $test = $package->make_test_from_coderef($checkSub, $name);
  $test->{_results} = \@normalResults;
  push(@tests, $test);

  # Now we do some ten runs with all murmur hash collisions.
  my @collideNames = (0..9);
  my @collideResults;
  for my $name (@collideNames) {
    my $testname = "${package}::testCompressWithCollisions$name";
    $test = $package->make_test_from_coderef(\&_testCompress, $testname);
    my $results = { testname => $testname };
    $test->{_name}      = "collide$name";
    $test->{_results}   = $results;
    $test->{_type}      = "collide";
    push(@tests, $test);
    push(@collideResults, $results);
  }

  $name = "${package}::checkResultsWithCollisions";
  $test = $package->make_test_from_coderef($checkSub, $name);
  $test->{_normalResults} = \@normalResults;
  $test->{_results} = \@collideResults;
  push(@tests, $test);

  my $suite = Test::Unit::TestSuite->empty_new($package);
  map { $suite->add_test($_) } @tests;
  return $suite;
}

########################################################################
# Run a test of compression.  These arguments are passed in $self and are
# set up in the suite method:
#
# $self->{_name} is a name used for the data set to be processed.
#
# $self->{_results} is a hashref, and VDO statistics are copied into here.
#
# $self->{_type} is "collide" for compressible data that are totally
# MurmurHash3 collisions, or "normal" for data that have identical
# compression expectations but without the hash collisions
##
sub _testCompress {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  my $stats = $device->getVDOStats();
  assertEqualNumeric(0, $stats->{"compressed fragments in packer"});
  assertEqualNumeric(0, $stats->{"current VDO IO requests in progress"});
  assertEqualNumeric(0, $stats->{"current dedupe queries"});
  assertEqualNumeric(0, $stats->{"data blocks used"});
  assertEqualNumeric(0, $stats->{"dedupe advice valid"});
  assertEqualNumeric(0, $stats->{"dedupe advice stale"});
  assertEq("normal", $stats->{"operating mode"});

  # Generate a block that has incredible compressibility because it
  # consists of many copies of the same 32 byte fragment.
  my $pathData = makeFullPath($machine->getScratchDir(), $self->{_name});
  $machine->runSystemCmd("echo ABDCDEFGHIJKLMNOPQRSTUVWXYZ012345 >$pathData");
  $machine->dd(
               if    => $pathData,
               of    => $pathData,
               count => int($self->{blockSize} / 32 - 1),
               bs    => 32,
               seek  => 1,
              );

  # Increase the block count to a multiple of the system page size.
  my $pageSize = $machine->getPageSize();
  my $blocksPerPage = int($pageSize / $self->{blockSize});
  assertEqualNumeric($pageSize, $blocksPerPage * $self->{blockSize});
  my $pageCount = int(($self->{blockCount} - 1) / $blocksPerPage) + 1;
  $self->{blockCount} = $pageCount * $blocksPerPage;

  # Generate a data file that has a single hash signature, but also has
  # incredible compressibility.  The single hash signature is provided by
  # the murmur3 collision magic (coded in murmur3collide).  The incredible
  # compressibility is due to each block consisting of many copies of only
  # two 32 byte fragments.
  $machine->murmur3collide(
                           if    => $pathData,
                           of    => $pathData,
                           count => $self->{blockCount} - 1,
                           bs    => $self->{blockSize},
                           seek  => 1,
                          );

  # If we are running a "normal" test, destroy the single hash signature
  # property of the data file.  It is important that this does not alter
  # the compressibility of the data blocks.
  if ($self->{_type} ne "collide") {
    my $pathTemp = makeFullPath($machine->getScratchDir(), "temp");
    $machine->runSystemCmd("mv $pathData $pathTemp");
    $machine->runSystemCmd("tr Q q <$pathTemp >$pathData");
    $machine->runSystemCmd("rm $pathTemp");
  }

  # Write the data to VDO.  Use the page cache to get many blocks in progress,
  # but write in page sized chunks.
  my $startTime = time();
  $device->ddWrite(
                   if    => $pathData,
                   count => $pageCount,
                   bs    => $pageSize,
                   conv  => "fdatasync",
                  );
  my $duration = time() - $startTime;
  my $rate = sizeToText($self->{blockSize} * $self->{blockCount} / $duration);
  $duration = timeToText($duration);
  $log->info("Writing $self->{blockCount} blocks took $duration at $rate/sec");

  # Wait for VDO to get every incomplete block into the packer.
  for (;;) {
    $stats = $device->getCurrentVDOStats();
    my $workingBlocks = ($stats->{"current VDO IO requests in progress"}
                         - $stats->{"compressed fragments in packer"});
    if ($workingBlocks == 0) {
      last;
    }
    $log->info("$workingBlocks blocks have not gotten to the packer yet");
    sleep(0.1);
  }

  # This call to get the VDOStats will have the side effect of flushing the
  # packer.
  $stats = $device->getVDOStats();
  # Check for dedupe timeouts first.  If there are any, the other expected
  # values will also be wrong.
  assertEqualNumeric(0, $stats->{"dedupe advice timeouts"});

  my $results = $self->{_results};
  foreach my $field (@ALL_STATS) {
    $results->{$field} = $stats->{$field};
  }
  $results->{"done"} = "done";

  # Merge hash lock dedupe and UDS index dedupe stats since the amount of each
  # is timing-dependent, but the sum should be stable.
  $results->{"dedupe matches (derived)"}
    = $stats->{"dedupe advice valid"} + $stats->{"concurrent data matches"};
  $results->{"dedupe mismatches (derived)"}
    = $stats->{"dedupe advice stale"} + $stats->{"concurrent hash collisions"};
}

########################################################################
# Assert that all the test runs have passed.
#
# @param $results  A listref of hashrefs.  Each hashref was passed to an
#                  earlier run of the _testCompress method.
##
sub _checkResultsPassed {
  my ($self, $results) = assertNumArgs(2, @_);

  # All tests must have passed
  foreach my $result (@$results) {
    assertDefined($result->{testname});
    assertDefined($result->{done}, "$result->{testname} failed");
    assertEq("done", $result->{done});
  }
}

########################################################################
# Check that the VDO statistics from different runs satisfy the test
# expectations.  These arguments are passed in $self and are set up in
# the suite method:
#
# $self->{_results} is a list of many hashrefs.  Each hashref was passed to
# an earlier run of the _testCompress method.
#
# $self->{_normalResults} is either undef or a list of many hashrefs.  Each
# hashref was passed to an earlier run of the _testCompress method using
# the "normal" type, and the hashrefs in $self->{_results} were passed to
# earlier runs of the _testCompress method using the "collide" type.
##
sub _checkResults {
  my ($self) = assertNumArgs(1, @_);
  my $results = $self->{_results};
  my $normalResults = $self->{_normalResults} || [];
  my @allResults = (@$results, @$normalResults);

  $self->_checkResultsPassed(\@allResults);

  foreach my $field (@SAME_STATS) {
    my @range = map { $_->{$field} } @allResults;
    my $value = $range[0];
    map { assertEq($value, $_, "$field should match") } @range;
  }

  foreach my $field (@RANGE_STATS) {
    my @range = map { $_->{$field} } @$results;
    my $mean = sum(@range) / scalar(@range);
    $log->info("Values for ${field}: " . join(" ", sort(@range)));
    $log->info("  Mean for ${field}: $mean");
    # Assert all the values are with 10% of the mean
    assertNear($mean, min(@range), "10%", $field);
    assertNear($mean, max(@range), "10%", $field);
    if (scalar(@$normalResults) > 0) {
      # We have both colliding and non-colliding results.
      # Assert that the mean values are within 5% of each other.
      #
      # The 5% threshold is chosen to make failures very rare.  The value
      # 4% results in 1 nightly test failure per month, and the value 3%
      # results in 3 nightly test failures per month.
      my @normalRange = map { $_->{$field} } @$normalResults;
      my $normalMean = sum(@normalRange) / scalar(@normalRange);
      assertNear($normalMean, $mean, "5%", $field);
    }
  }
}

1;
