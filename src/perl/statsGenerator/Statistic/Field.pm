##
# A Statistic which is a single field of a statistic structure.
#
# @synopsis
#
#     use Statistic::Field
#
#
# @description
#
# C<Statistic::Field> is an actual statistic or collection of them.
#
# $Id$
##
package Statistic::Field;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);

use Permabit::Assertions qw(
  assertMinArgs
  assertNumArgs
);

use base qw(Statistic);

##
# @paramList{new}
our %PROPERTIES
  = (
     # @ple the array size of this field (undef if the field is not an array)
     arraySize => undef,
    );
##

######################################################################
# @inherit
##
sub getType {
  my ($self, $language) = assertNumArgs(2, @_);
  return $self->{type}->getType($language);
}

######################################################################
# Get the array size of this field.
##
sub getArraySize {
  my ($self) = assertNumArgs(1, @_);
  return $self->{arraySize};
}

1;
