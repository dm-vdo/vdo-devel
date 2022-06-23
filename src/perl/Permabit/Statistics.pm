##
# Perl object that represents a set of statistics.  Three types:
#
# Type 0: A set of measurements with no associated time
#
# Type 1: A set of measurements sampled at the same point in time.
#
# Type 2: A set of differences between two Type 1 sets.  This represents
# the incremental differences between two points in time.
#
# This is a base class that implements the time part of the set.  The
# actual measurement samples are supplied by a subclass.
#
# $Id$
##
package Permabit::Statistics;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Carp qw(confess);
use List::Util qw(sum);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertMinArgs
  assertMinMaxArgs
  assertNotDefined
  assertNumArgs
  assertTrue
  assertType
);
use Permabit::Utils qw(parseISO8061toMillis timeToText);

# Overload subtraction to compute type 2 differences
use overload "-" => \&subtract;
# Overload multiplication and division for scaling
use overload "*" => \&multiply;
use overload "/" => \&divide;

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{new}
#
# Note that this is copied in new() without use of dclone(). The
# titleMap, typeMap, and unitsMap hashes would thus be shared by any
# created object, except that every subclass overrides them.
my %PROPERTIES
  = (
     # @ple For type 2, this is the time interval between the measurements.
     duration    => undef,
     # @ple Time at which we took measurements.  For type 1, this is the
     #      moment of sampling.  For type 2, this is the start time of the
     #      interval.
     timeStamp   => undef,
     # @ple The timeStamp converted to seconds since the epoch
     timeSeconds => undef,
     # @ple the hash mapping field names to titles
     titleMap    => {},
     # @ple the hash mapping field names to value types.  Values:
     #      "counter" is a monotonically increasing value,
     #      "snapshot" is a number that can change arbitrarily,
     #      "maximum" is the maximum ever seen for that value,
     #      "state" is a non-numeric state value,
     #      "constant" is an unchanging value (which need not be numeric), and
     #      "hash" is a hash mapping names to a C<Permabit::Statistics>.
     typeMap     => {},
     # @ple the hash mapping field names to units
     unitsMap    => {},
    );
##

#############################################################################
# Creates a C<Permabit::Statistics>. C<new> optionally takes arguments, in
# the form of key-value pairs.
#
# @params{new}
#
# @return a new C<Permabit::Statistics>
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self = bless { %PROPERTIES,
                     # Overrides previous values
                     @_,
                   }, $class;
  if (defined($self->{timeStamp}) && !defined($self->{timeSeconds})) {
    $self->{timeSeconds} = parseISO8061toMillis($self->{timeStamp}) / 1000;
  }
  return $self;
}

#############################################################################
# Creates a C<Permabit::Statistics> by computing the average values from a
# list of C<Permabit::Statistics> instances.
#
# @param stats    At least two sets of stats
#
# @return the average stats
##
sub average {
  my @stats = assertMinArgs(2, @_);
  my $first = $stats[0];
  my $last = $stats[-1];
  my $timeStamp = undef;

  my $firstRef = ref($first);
  for my $s (@stats) {
    # N.B.: Calling a bunch of assert* functions for each of millions
    # of entries in a run can get very expensive, hence the explicit
    # checks here instead.
    #
    # Ensure that we are averaging objects of the same type
    my $sRef = ref($s);
    if ($firstRef ne $sRef) {
      confess("stats to average not all the same types"
              . " ('$firstRef', '$sRef')");
    }
    # Ensure that we are averaging type 1 objects
    if (!defined($s->{timeStamp})) {
      confess("stats to average missing timestamp");
    }
    if (defined($s->{duration})) {
      confess("stats to average already have duration computed");
    }
    # Ensure that the time stamps are in the correct order
    if (defined($timeStamp) && !($timeStamp lt $s->{timeStamp})) {
      confess("stats to average not in chronological order"
              . "($timeStamp ... $s->{timeStamp})");
    }
    $timeStamp = $s->{timeStamp};
  }
  # Generate the type 2 object information
  my %ma;
  $ma{duration}    = $last->{timeSeconds} - $first->{timeSeconds};
  $ma{timeSeconds} = $first->{timeSeconds};
  $ma{timeStamp}   = $first->{timeStamp};

  # Generate the average values of the snapshots, and the difference in the
  # counters across the time interval.
  while (my ($field, $type) = each(%{$first->{typeMap}})) {
    if ($type eq "constant") {
      # N.B.: See above re expensive assertion calls.
      if ($first->{$field} ne $last->{$field}) {
        confess("constant field $field changes across stats"
                . "($first->{$field}, $last->{$field})");
      }
      $ma{$field} = $last->{$field};
    } elsif ($type eq "counter") {
      if (($first->{$field} eq "N/A") || ($last->{$field} eq "N/A")) {
        $ma{$field} = "N/A";
      } else {
        $ma{$field} = $last->{$field} - $first->{$field};
      }
    } elsif ($type eq "maximum") {
      $ma{$field} = $last->{$field};
    } elsif ($type eq "snapshot") {
      my @defStats = grep { defined($_) } @stats;
      if (scalar(@defStats) == 0) {
        $ma{$field} = "N/A";
      } else {
        $ma{$field} = sum(map { $_->{$field} } @defStats) / scalar(@defStats);
      }
    } elsif ($type eq "hash") {
      $ma{$field} = {};
      for my $subfield (keys(%{$first->{$field}})) {
        $ma{$field}{$subfield}
          = average(map { $_->{$field}{$subfield} } @stats);
      }
    }
  }
  return $first->new(%ma);
}

#############################################################################
# Creates a C<Permabit::Statistics> by computing the differences between two
# C<Permabit::Statistics> instances.
#
# @param  s2  The later stats
# @param  s1  The earlier stats
#
# @return the difference s2 - s1
##
sub subtract {
  my $s2 = shift;
  my $s1 = shift;
  my %diff;
  # Ensure that we are subtracting two objects of the same type
  assertEq(ref($s2), ref($s1));
  # If we have two timestamps, ensure that we are subtracting two type 1
  # objects and generate the type 2 object information.  Otherwise just
  # drop the timestamps.
  if (defined($s1->{timeStamp}) && defined($s2->{timeStamp})) {
    # Ensure that the time stamps are in the correct order
    assertTrue($s1->{timeStamp} lt $s2->{timeStamp});
    assertNotDefined($s2->{duration});
    assertNotDefined($s1->{duration});
    $diff{duration}    = $s2->{timeSeconds} - $s1->{timeSeconds};
    $diff{timeSeconds} = $s1->{timeSeconds};
    $diff{timeStamp}   = $s1->{timeStamp};
  }
  # Generate the differences in the statistics values
  while (my ($field, $type) = each(%{$s2->{typeMap}})) {
    if ($type eq "constant") {
      assertEq($s1->{$field}, $s2->{$field}, $field);
      $diff{$field} = $s2->{$field};
    } elsif (($type eq "counter") || ($type eq "snapshot")) {
      assertDefined($s1->{$field}, $field);
      assertDefined($s2->{$field}, $field);
      if (($s1->{$field} eq "N/A") || ($s2->{$field} eq "N/A")) {
        $diff{$field} = "N/A";
      } else {
        $diff{$field} = $s2->{$field} - $s1->{$field};
      }
    } elsif ($type eq "maximum") {
      assertDefined($s1->{$field}, $field);
      assertDefined($s2->{$field}, $field);
      $diff{$field} = $s2->{$field};
    } elsif ($type eq "hash") {
      $diff{$field} = {};
      for my $subfield (keys(%{$s2->{$field}})) {
        if (exists($s1->{$field}{$subfield})) {
          $diff{$field}{$subfield}
            = $s2->{$field}{$subfield} - $s1->{$field}{$subfield};
        } else {
          $diff{$field}{$subfield} = $s2->{$field}{$subfield};
        }
      }
    }
  }
  return $s2->new(%diff);
}

#############################################################################
# Scale the values by a specified multiplier.
#
# @param  stats       The stats
# @param  multiplier  The multiplier
# @param  swapped     Whether the arguments were swapped (implicitly added)
#
# @return the scaled stats
##
sub multiply {
  my ($stats, $multiplier, $swapped) = assertNumArgs(3, @_);
  my %product;

  assertType(__PACKAGE__, $stats);
  # Assert multiplier is numeric?  Or let the multiply blow up.
  $product{timeStamp} = $stats->{timeStamp};
  $product{duration} = $stats->{duration};
  $product{timeSeconds} = $stats->{timeSeconds};

  while (my ($field, $type) = each(%{$stats->{typeMap}})) {
    if ($type eq "constant") {
      $product{$field} = $stats->{$field};
    } elsif (($type eq "counter")
             || ($type eq "maximum")
             || ($type eq "snapshot")) {
      if ($stats->{$field} eq "N/A") {
        $product{$field} = "N/A";
      } else {
        $product{$field} = $stats->{$field} * $multiplier;
      }
    } elsif ($type eq "hash") {
      $product{$field} = {};
      for my $subfield (keys(%{$stats->{$field}})) {
        $product{$field}{$subfield}
          = $stats->{$field}{$subfield} * $multiplier;
      }
    }
  }
  return $stats->new(%product);
}

#############################################################################
# Divide the values by a specified factor.
#
# @param  stats    The stats
# @param  divisor  The divisor
# @param  swapped  Whether the arguments were swapped (implicitly added)
#
# @return the scaled stats
##
sub divide {
  my ($stats, $divisor, $swapped) = assertNumArgs(3, @_);
  return multiply($stats, 1 / $divisor, $swapped);
}

#############################################################################
# Extract the time information from the statistics, so that they can be
# applied to a contained set of statistics.
#
# @return Time information in the form to be passed to the new() method
##
sub extractTime {
  my ($self) = assertNumArgs(1, @_);
  return (
          duration    => $self->{duration},
          timeSeconds => $self->{timeSeconds},
          timeStamp   => $self->{timeStamp},
         );
}

#############################################################################
# Get the value from a field.  The field name is either the name of a
# member of the hash, or is a method name that can be called to retrieve
# the value.
#
# @param field  Field name
#
# @return field value
##
sub getField {
  my ($self, $field) = assertNumArgs(2, @_);
  return $self->can($field) ? $self->$field() : $self->{$field};
}

#############################################################################
# Get the title for a field.  The title is used to title a line on a graph,
# and should describe what the field measures.
#
# @param field  Field name
#
# @return field title
##
sub getTitle {
  my ($self, $field) = assertNumArgs(2, @_);
  return $self->{titleMap}{$field} || $field;
}

#############################################################################
# Get the units that a field is measured in.  This value can be used to
# label an axis on a graph.  Please do not get too creative here, as we can
# plot multiple fields on the same graph if they use the same label.
#
# @param field  Field name
#
# @return field label
##
sub getUnits {
  my ($self, $field) = assertNumArgs(2, @_);
  return $self->{unitsMap}{$field};
}

#############################################################################
# Log the statistics
#
# @param  label   Label logged with the statistics
# @oparam prefix  Prefix string used to indent the log messages
##
sub logStats {
  my ($self, $label, $prefix) = assertMinMaxArgs([""], 2, 3, @_);
  $log->info("$prefix$label");
  if (defined($self->{timeStamp})) {
    $log->info("$prefix  timeStamp => $self->{timeStamp}");
  }
  if (defined($self->{duration})) {
    $log->info("$prefix  duration  => " . timeToText($self->{duration}));
  }
}

1;
