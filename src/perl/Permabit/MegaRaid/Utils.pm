#############################################################################
# Utility class for common MegaRaid related tasks
#
# $Id$
##
package Permabit::MegaRaid::Utils;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use File::Path;
use Storable qw(dclone);

use Permabit::Assertions qw(
  assertDefined
  assertNumDefinedArgs
  assertTrue
  assertLTNumeric
);
use Permabit::MegaRaid::MegaCli;
use Permabit::SystemUtils qw(
  assertCommand
  runCommand
);

use base qw(Exporter);

our @EXPORT_OK = qw(
  parseCliResult
  chunkCliOut
);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# Given a string, and a hash ref of { wanted-key => key-to-use-in-result-hash },
#  parse the string, extract groups of wanted key => value pairs, and
#  return an array ref of these hash groups.  Please see example below.
#
# Example:
#  my $str = "a : 1\n"
#          . "c : 2\n"
#          . "e : 3\n"
#          . "a : 4\n"
#          . "c : 5\n";
#
# parseCliResult($str, { a => y, c => z })
# returns [ { y => 1, z => 2 }, { y => 4, z => 5 } ]
#
# @param cliOutput       String containing MegaCli output to be parsed.
# @param wanted          A hash ref of wanted keys, who's values will be converted
#                         keys containing the results.
#
# @return an array ref of the hash refs we created
# @croaks if multiple occurances of the same key is found before a group is complete
#           or result array is empty
##
sub parseCliResult {
  my ($cliOutput, $wanted) = assertNumDefinedArgs(2, @_);
  my @results;
  my %hash = ();

  # do processing -- add to @results
  foreach my $line (split(/\n/, $cliOutput)) {
    my ($key, $value) = _parseCliOutLine($line, [ keys %{$wanted} ]);
    # next if we don't find a (key, value) pair we care about
    if (! defined($key) || ! defined($value)) {
      next;
    }
    # set the key we'll use in our results
    my $resultKey = $wanted->{$key};
    # check if we've already seen this key in this group (ERROR)
    if (defined($hash{$resultKey})) {
      # 'key' is more relevent here than resultKey, for debugging purposes
      $log->logcroak("Duplicate key: $key");
    }
    $hash{$resultKey} = $value;
    # Check to see if our group is complete
    if (scalar(keys %hash) == scalar(keys %{$wanted})) {
      my %tmp = %hash;
      push(@results, \%tmp);
      %hash = ();
      next;
    }
  }

  # assert that we're not returning an empty list
  assertLTNumeric(0, scalar(@results), "Could not find all keys: "
                                       . join(", ", keys %{$wanted}) . "\n"
                                       . "Given input: $cliOutput");

  return \@results;
}

#############################################################################
# Given Cli output lines, create output line chunks, each starting with
#  a given string.
#
# @param cliOut       String containing lines of MegaCli output to split
# @param chunkStart   The delimiter
#
# @return the array ref of line groups
##
sub chunkCliOut {
  my ($cliOut, $chunkStart) = assertNumDefinedArgs(2, @_);

  my @allLines     = split("\n", $cliOut);
  my $groupIndx    = 0;
  my @textGroups   = ();
  my $chunkStarted = 0;
  foreach my $line (@allLines) {
    if ($line =~ /$chunkStart/) {
      $chunkStarted = 1;
      $groupIndx   += $textGroups[$groupIndx] ? 1 : 0;
      next;
    }
    $textGroups[$groupIndx] .= $chunkStarted ? "$line\n" : "";
  }

  return \@textGroups;
}

#############################################################################
# Given a single line of megacli output, and an array ref of wanted keys,
#  split the line into a key => value pair.  If the key is one we wanted,
#  return both the key and the value in an array.  If we did not find a
#  key we want, return undef.
#
# @param cliLine      String containing a line of MegaCli output
# @param wanted       Array ref of wanted keys
#
# @return the key, value array or undef
##
sub _parseCliOutLine {
  my ($cliLine, $wanted) = assertNumDefinedArgs(2, @_);
  # The output is in 'key : value' format
  my $candidate = qr/^([^:]+):([^:]+)$/;
  if ($cliLine =~ $candidate) {
    my ($key, $value) = ($1, $2);
    # strip leading and trailing spaces
    $key   =~ s/^\s+|\s+$//g;
    $value =~ s/^\s+|\s+$//g;
    if (! grep { /^$key$/ } @{$wanted}) {
      # return undef because this key is not defined in @wanted
      return undef;
    }
    return $key, $value;
  }
  return undef;
}

1;
