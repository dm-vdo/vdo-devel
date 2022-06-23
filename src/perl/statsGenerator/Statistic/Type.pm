##
# A Statistic which defines a statistics type or set of types.
#
# @synopsis
#
#     use Statistic::Type
#
#
# @description
#
# C<Statistic::Type> is statistic type or set of types.
#
# $Id$
##
package Statistic::Type;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);

use Permabit::Assertions qw(assertNumArgs);

use base qw(Statistic);

######################################################################
# @inherit
##
sub getType {
  my ($self, $language) = assertNumArgs(2, @_);
  return ($self->getAttribute($language) // $self->{name});
}

######################################################################
# @inherit
##
sub addToStatistic {
  my ($self, $other, $swap) = assertNumArgs(3, @_);

  my $otherType = ref($other);
  if (($otherType eq 'Statistic::Type')
      || ($otherType eq 'Statistic::Struct')) {
    $self->addAttribute($other->{name}, $other);
    return $self;
  }

  if (($otherType eq 'Attribute') || ($otherType eq 'ARRAY')) {
    $self->SUPER::addToStatistic($other, $swap);
    return $self;
  }

  die("Can't add $otherType to " . ref($self));
}

1;
