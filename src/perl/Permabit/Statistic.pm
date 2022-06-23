##
# An incremental accumulator of statistical information about a
# sampled variable.
#
# $Id$
##
package Permabit::Statistic;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp qw(croak);
use Permabit::Assertions qw(assertNumArgs);

#############################################################################
# @paramList{new}
my %properties = (
  # @ple The number of samples
  _count      => 0,
  # @ple The maximum value of all the samples
  _max        => undef,
  # @ple The minimum value of all the samples
  _min        => undef,
  # @ple The sum of the samples
  _sum        => 0,
  # @ple The sum of the square of every sample
  _sumSquares => 0,
);
##

#############################################################################
# Creates a C<Permabit::Statistic>.
#
# @params{new}
#
# @return a new C<Permabit::Statistic>
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  return bless { %properties,
                 # Overrides previous values
                 @_,
               }, $class;
}

#############################################################################
# Return the number of samples
#
# @return the number of samples
##
sub count {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_count};
}

#############################################################################
# Return the maximum value of the samples
#
# @return the maximum value of the samples
##
sub max {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_max};
}

#############################################################################
# Return the sample mean (average) of all the samples.
#
# @return the arithmetic mean of the samples
#
# @croaks if there have been no samples
##
sub mean {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->{_count}) {
    croak("No samples");
  }
  return $self->{_sum} / $self->{_count};
}

#############################################################################
# Return the minimum value of the samples
#
# @return the minimum value of the samples
##
sub min {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_min};
}

#############################################################################
# Include a sampled value of the variable in this statistic.
#
# @param value  The sampled value
##
sub sample {
  my ($self, $value) = assertNumArgs(2, @_);
  if ($self->{_count}) {
    if ($value > $self->{_max}) {
      $self->{_max} = $value;
    }
    if ($value < $self->{_min}) {
      $self->{_min} = $value;
    }
  } else {
    $self->{_max} = $value;
    $self->{_min} = $value;
  }
  $self->{_count}++;
  $self->{_sum} += $value;
  $self->{_sumSquares} += $value * $value;
}

#############################################################################
# Return an unbiased estimate of the standard deviation based on the
# sampled values.
#
# @return the sample standard deviation
#
# @croaks if there have been less than 2 samples
##
sub sigma {
  my ($self) = assertNumArgs(1, @_);
  my $count = $self->{_count};
  if ($count < 2) {
    croak("No samples");
  }
  my $sum        = $self->{_sum};
  my $sumSquares = $self->{_sumSquares};
  # The sum of the squares of the deviations from an expected value is
  # equal to the sum of the squares of the values minus the sum of the
  # squares of the expected value.
  # Note that (EV * EV * N) = (sum * sum) / N.
  return sqrt(($sumSquares - (($sum * $sum) / $count)) / ($count - 1));
}

#############################################################################
# Return the standard deviation of all the samples. This assumes that
# the entire population has been sampled.
#
# @return the standard deviation
#
# @croaks if there have been no samples
##
sub standardDeviation {
  my ($self) = assertNumArgs(1, @_);
  my $count = $self->{_count};
  if (!$count) {
    croak("No samples");
  }
  my $sum = $self->{_sum};
  # The sum of the squares of the deviations from an expected value is
  # equal to the sum of the squares of the values minus the sum of the
  # squares of the expected value.
  # Note that (EV * EV * N) = (sum * sum) / N.
  return sqrt(($self->{_sumSquares} - ($sum * $sum) / $count) / $count);
}

#############################################################################
# Return the sum of all sample valiues.
#
# @return the sum of all the samples
##
sub sum {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_sum};
}

1;
