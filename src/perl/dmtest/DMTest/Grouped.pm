##
# Base class for tests which run dmtest python tests.  Runs all the
# tests quickly grouped into a single testcase.
#
# $Id$
##
package DMTest::Grouped;

use strict;
use warnings FATAL => qw(all);
use Data::Dumper;
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertFalse
  assertNumArgs
  assertMinMaxArgs
  assertRegexpDoesNotMatch
  assertRegexpMatches
);
use Permabit::CommandString::DMTest;
use Permabit::Utils qw(yamlStringToHash);

use base qw(DMTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

######################################################################
# Return a hashref representing the output returned from "dmtest list".
# The output is in the following form:
#
# vdo
#   create
#     create01                  result [time]
#     create02                  result [time]
#   rebuild
#     rebuild01                 result [time]
#
# The leaf nodes in the hash will have a value containing the result
# and time the test took or '-' if it has not been run yet.
# 
#
# @param tree  The tree returned from dmtest list.
#
# @return      The hashref representing the tree.
##
sub treeToHashes {
  my ($self, $tree) = assertNumArgs(2, @_);
  assertDefined($tree, "no tree to parse");
  my %tests;
  while( $tree =~ /^( *)(.*)\n((?:\1 +.*\n)*)/gm ) {
    my ($head, $rest) = ($2, $3);
    $head =~ s/ ->.*//;
    $head =~ s/^\s+|\s+$//g;
    if ($rest) {
      $tests{$head} = $self->treeToHashes($rest);
    } else {
      if ($head =~ /([^\s]*)\s*([^\s]*.*)/gm) {
        my ($test, $value) = ($1, $2);
        $tests{$test} = $value;
      } else {
        $tests{$head} = undef;
      }
    }
  }
  return \%tests;
}

######################################################################
# Return an array of strings that can be used for dmtest calls.
# The strings will be in the following form:
#
# vdo/create/create01
# vdo/create/create02
# vdo/rebuild/rebuild01
#
# This function calls itself recursively to build up the strings.
#
# @param data  The hashref representing the tree
#
# @return      The array of test paths to use in dmtest
##
sub hashesToTests {
  my ($self, $data, $test) = assertNumArgs(3, @_);
  assertDefined($data, "no data to format");
  my @tests;
  if (ref $data eq ref {}) {
    foreach my $key (keys %{ $data }) {
      push(@tests, $self->hashesToTests($data->{$key},
                                        $test eq "" ? "$key" : "$test/$key"));
    }
  } else {
    push(@tests, $test);
  }
  return @tests;
}

######################################################################
# Return the result and elapsed time of a single test.
#
# @param data  The hashref representing the tree
#
# @return      The result and elapsed time of the test
##
sub hashesToResult {
  my ($self, $data, $test) = assertNumArgs(3, @_);
  assertDefined($data, "no data to retrieve results from");
  # using split function without Limit 
  my @keys = split('/', $test);
  # displaying string after splitting
  foreach my $key (@keys) {
    $data = $data->{$key};
  }
  if ($data =~ /(PASS|FAIL|MISSING_DEP)\s*(\[(.*)\])?/gm) {
    return ($1, defined($3) ? $3 : "-");
  }
  return ("-", "-");
}

#############################################################################
##
sub testRunner {
  my ($self) = assertNumArgs(1, @_);

  my $dmtestRun = $self->runTests($self->{dmtestName});
  my $hashes = $self->treeToHashes($dmtestRun);
  my @tests = $self->hashesToTests($hashes, "");
  my $lastError;
  # After all the tests have run, we will produce a FAILURE message
  # naming each test that does not pass, if there are any.
  my @failures;
  foreach my $test (@tests) {
    eval {
    # Run the next test
      my ($result, $elapsed) = $self->hashesToResult($hashes, $test);
      assertRegexpMatches(qr/^PASS$/, $result);
    };
    if ($EVAL_ERROR) {
      my $testError = $EVAL_ERROR;
      # The test failed.  Log and record the failure.
      $lastError = "$test: $testError";
      $log->fatal($testError);
      $log->fatal($self->listTestLogs($test));
      push(@failures, $test);
    }
  }
  if (scalar(@failures) > 0) {
    if (scalar(@failures) == 1) {
      die($lastError);
    }
    die(join(" ", @failures));
  }
}

1;
