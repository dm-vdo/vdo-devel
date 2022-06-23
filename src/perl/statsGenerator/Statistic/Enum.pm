##
# A C<Statistic> which is a set of enumerated constants.
#
# @synopsis
#
#     use Statistic::Enum
#
# @description
#
# C<Statistic::Enum> is a statistic of enumerated constants. It replaces the
# attributes hash in the base class with an indexed hash so that the enum
# can be generated in the proper order.
#
# $Id$
##
package Statistic::Enum;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);
use Tie::IxHash;

use Permabit::Assertions qw(
  assertMinArgs
  assertMinMaxArgs
  assertNumArgs
);

use base qw(Statistic);

##
# @paramList{new}
our %PROPERTIES
  = (
     # @ple The name of the statistic
     name       => undef,
     # @ple The type of the statistic
     type       => 'enum',
     # @ple Named attributes of the statistic
     attributes => Tie::IxHash->new(),
    );
##

######################################################################
# @inherit
##
sub getAttribute {
  my ($self, $name, $mayInherit) = assertMinMaxArgs([0], 2, 3, @_);
  my $value = $self->{attributes}->FETCH($name);
  if (!defined($value) && $mayInherit && defined($self->{parent})) {
    return $self->{parent}->getAttribute($name, 1);
  }

  return $value;
}

######################################################################
# Add a new attribute to a Statistic
#
# @param name  The name of the attribute
# @param value The value of the attribute
##
sub addAttribute {
  my ($self, $name, $value) = assertNumArgs(3, @_);
  if (defined($self->getAttribute($name))) {
    die("redefinition of attribute $name in $self->{name}\n");
  }

  $self->{attributes}->STORE($name, $value);
}

######################################################################
# Get all of the enumerants in an Enum.
#
# @return The names of the enumerated constants
##
sub getEnumerants {
  my ($self) = assertNumArgs(1, @_);
  return $self->{attributes}->Keys();
}

######################################################################
# Get a hash of the enumerants in an Enum.
#
# @return A hashref containing the names and values of the Enum.
##
sub asHash {
  my ($self) = assertNumArgs(1, @_);
  return { map({ ($_, $self->getAttribute($_)) } $self->getEnumerants()) };
}

1;
