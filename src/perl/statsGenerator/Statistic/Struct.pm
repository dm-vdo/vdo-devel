##
# A Statistic which is a statistic or collection of same.
#
# @synopsis
#
#     use Statistic::Statistic
#
#
# @description
#
# C<Statistic::Statistic> is an actual statistic or collection of them.
#
# $Id$
##
package Statistic::Struct;

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

1;
