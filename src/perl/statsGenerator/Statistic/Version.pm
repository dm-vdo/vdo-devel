##
# A Statistic which is a statistics version number
#
# @synopsis
#
#     use Statistic::Version
#
#
# @description
#
# C<Statistic::Version> is a statistic of enumerated constants
#
# $Id$
##
package Statistic::Version;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);

use Permabit::Assertions qw(
  assertNumArgs
);

use base qw(Statistic::Enum);

##
# @paramList{new}
our %PROPERTIES
  = (
     # @ple The name of the statistic
     name => 'version',
    );
##

######################################################################
# Construct a new Statistic::Version.
#
# @params{new}
##
sub new {
  my ($invocant, $number) = assertNumArgs(2, @_);
  my $self                = $invocant->SUPER::new();
  $self->addAttribute('STATISTICS_VERSION', $number);
  return $self;
}

1;
