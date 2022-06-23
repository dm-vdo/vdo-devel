######################################################################
# An instance of an Albireo process.
#
# This class encapsulates the extra command line magic to use
# binaries that are linked against the UDS library.
#
# $Id$
##
package Permabit::AlbireoCommand;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp;
use Log::Log4perl;
use Permabit::AlbireoTestUtils qw(
                                   addAlbireoEnvironment
                                );
use Permabit::Assertions qw(
                             assertNumArgs
                             assertMinMaxArgs
                          );

use base qw(Permabit::GenericCommand);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %PROPERTIES =
  (
   # target indexer
   indexer         => undef,
  );

my %INHERITED_PROPERTIES =
  (
   # index directory
   indexDir        => undef,
   # whence binaries are run
   binaryDir => undef,
  );

#############################################################################
# @inherit
##
sub getProperties {
  my $self = shift;
  return ( $self->SUPER::getProperties(),
           %PROPERTIES );
}

#############################################################################
# @inherit
##
sub getInheritedProperties {
  my $self = shift;
  return ( $self->SUPER::getInheritedProperties(),
           %INHERITED_PROPERTIES );
}

######################################################################
# Generates the name of the logfile to use for the albireo library
# debug log.
#
# @return              The logfile string.
##
sub getLogfileName {
  my ($self) = assertNumArgs(1, @_);
  return "$self->{name}.log";
}

######################################################################
# @inherit
##
sub buildEnvironment {
  my ($self) = assertNumArgs(1, @_);
  addAlbireoEnvironment($self->{env},
                        $self->{binaryDir},
                        $self->getLogfileName());
  return $self->SUPER::buildEnvironment();
}

1;
