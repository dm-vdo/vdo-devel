#############################################################################
# An instance of an albtest process.
#
# $Id$
##
package Permabit::Albtest;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(
  assertNumArgs
);

use base qw(Permabit::AlbireoProfilingCommand);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %PROPERTIES =
  (
   # Whether to run gprof
   gprof    => 0,
   # Executable name
   name     => 'albtest',
   # Test name
   testName => undef,
  );

######################################################################
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
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  my @args;
  if ($self->{binary} =~ /albtest|vdotest/) {
    if ($self->{valgrind}) {
      @args = ("--no-unload --no-fork");
    }
    push(@args, $self->{testName});
  } else {
    if ($self->{gprof}) {
      @args = ("&& gprof $self->{binary}");
    }
  }
  return @args;
}

#############################################################################
# @inherit
##
sub as_string {
  my $self = shift;
  return "(Albtest on $self->{host})";
}

1;
