##############################################################################
# A command for running the vdostats command
#
# $Id$
##
package Permabit::CommandString::VDOStats;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(Permabit::CommandString);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %COMMANDSTRING_PROPERTIES
  = (
     # string of the vdo device(s) to query; undef => all devices
     deviceName => undef,
     # executable name
     name       => "vdostats",
     # verbose output
     verbose    => undef,
    );

our %COMMANDSTRING_INHERITED_PROPERTIES
  = (
     # The path to the binaries; allows parent to be a Managed vdo;
     # appended first to path
     albireoBinaryPath => undef,
     # Name of the VDO device to operate on
     deviceName        => undef,
    );

###############################################################################
# @inherit
##
sub getEnvironment {
  my ($self) = assertNumArgs(1, @_);
  my $path = "\$PATH";
  if (defined($self->{albireoBinaryPath})) {
    $path = "$path:$self->{albireoBinaryPath}";
  }
  if (defined($self->{binaryDir})) {
    $path = "$path:$self->{binaryDir}";
  }
  $self->{env}->{PATH} = $path;

  return $self->SUPER::getEnvironment();
}

###############################################################################
# @inherit
##
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  my @SPECIFIERS = qw(
    deviceName
    all?
    human-readable?
    si?
    verbose?
    version?
  );

  return $self->SUPER::getArguments(@SPECIFIERS);
}

1;
