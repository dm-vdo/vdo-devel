##
# Base class for objects parsed from a .stats file.
#
# @synopsis
#
#     use base qw(Statistic);
#
# @description
#
# C<Statistic> is the base class for objects which consist of named attributes
# and children.
#
# $Id$
##
package Statistic;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);

use Permabit::Assertions qw(
  assertMinArgs
  assertMinMaxArgs
  assertNumArgs
);

# Overload stringification to print something meaningful
use overload q("") => \&as_string;
use overload q(+)  => \&add;

use base qw(Permabit::Propertied);

##
# @paramList{new}
our %PROPERTIES
  = (
     # @ple children of the statistic
     children   => [],
     # @ple The name of the statistic
     name       => undef,
     # @ple The parent of the statistic
     parent     => undef,
     # @ple Named attributes of the statistic
     attributes => {},
     # @ple the type of this statistic
     type       => undef,
    );
##

######################################################################
# Get the parent of this statistic.
#
# @return The parent of this statistic
##
sub getParent {
  my $self = assertNumArgs(1, @_);
  return $self->{parent};
}

######################################################################
# Check whether this statistic has a child with a given name.
#
# @param childName  The name of the child
#
# @return True if this statistic has a child with the given name
##
sub hasChildren {
  my ($self, $childName) = assertNumArgs(2, @_);
  return scalar( grep { $_->{name} eq $childName } @{$self->{children}} );
}

######################################################################
# Get the children of this statistic.
#
# @return The children of this statistic
##
sub getChildren {
  my ($self) = assertNumArgs(1, @_);
  return @{$self->{children}};
}

######################################################################
# Add a new child to a Statistic.
#
# @param child  The child to add
##
sub addChild {
  my ($self, $child) = assertNumArgs(2, @_);
  my $childName = $child->{name};
  if ($self->hasChildren($childName)) {
    die("redefinition of $childName in $self->{name}\n");
  }

  push(@{$self->{children}}, $child);
  $child->setParent($self);
}

######################################################################
# Get the value of a named attribute of this statistic.
#
# @param  name        The name of the attribute
# @oparam mayInherit  If true, will recursively search this statistic's parent
#                     if the attribute is not defined for the statistic itself
#
# @return The value of the attribute or undef if the attribute is not
#         defined for this statistic
##
sub getAttribute {
  my ($self, $name, $mayInherit) = assertMinMaxArgs([0], 2, 3, @_);
  my $value = $self->{attributes}{$name};
  if (!defined($value) && $mayInherit && defined($self->{parent})) {
    return $self->{parent}->getAttribute($name, 1);
  }

  return $value;
}

######################################################################
# Check whether a named attribute of this statistic has a specified value.
#
# @param  name        The name of the attribute
# @param  expected    The expected value of the attribute
# @oparam mayInherit  If true, will recursively search this statistic's parent
#                     if the attribute is not defined for the statistic itself
#
# @return True if this statistic's value for the given attribute matches the
#         expectation.
##
sub checkAttribute {
  my ($self, $name, $expected, $mayInherit) = assertMinMaxArgs([0], 3, 4, @_);
  my $value = $self->getAttribute($name, $mayInherit);
  return (defined($value) && ($value eq $expected));
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

  $self->{attributes}{$name} = $value;
}

######################################################################
# Set the parent of this statistic.
#
# @param parent  The parent
##
sub setParent {
  my ($self, $parent) = assertNumArgs(2, @_);
  if (defined($self->{parent})) {
    die("$self already has a parent");
  }

  $self->{parent} = $parent;
}

######################################################################
# @inherit
##
sub as_string {
  # Overload default stringification to print our name
  return $_[0]->{name};
}

######################################################################
# @inherit
##
sub add {
  # Overload addition to do appropriate things, depending upon the precise
  # types being added.
  my ($self, $other, $swap) = assertNumArgs(3, @_);
  if ($swap) {
    die("Strange swapped addition for $self and $other");
  }
  $self->addToStatistic($other, $swap);
}

######################################################################
# Overload addition
##
sub addToStatistic {
  my ($self, $other, $swap) = assertNumArgs(3, @_);

  my $otherType = ref($other);
  if ($otherType eq 'Statistic') {
    $self->addChild($other);
    return $self;
  }

  if ($otherType eq 'Statistic::Version') {
    $self->addAttribute($other->{name}, $other);
    return $self;
  }

  if ($otherType =~ /^Statistic::Type/) {
    # don't actually add type statistics, they are tracked seperately.
    die("Types can't be added");
  }

  if ($otherType =~ /^Statistic::/) {
    $self->addChild($other);
    return $self;
  }

  if ($otherType eq 'Attribute') {
    $self->addAttribute(@{$other});
    return $self;
  }

  if ($otherType eq 'ARRAY') {
    foreach my $addend (@{$other}) {
      $self += $addend;
    }
    return $self;
  }

  die("Can't add $otherType to " . ref($self));
}

1;
