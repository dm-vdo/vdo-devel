##
# Base class for UDS tests.
#
# $Id$
##
package UDSTest;

use strict;
use warnings FATAL => qw(all);
use Carp;
use Cwd qw(cwd);
use English qw(-no_match_vars);
use File::Basename;
use File::Path;
use Log::Log4perl;

use Permabit::Assertions qw(assertDefined assertNumArgs);
use Permabit::RemoteMachine;

use feature qw(state);

use base qw(Permabit::Testcase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %PROPERTIES
  = (
     # @ple what class of machine to run the test on
     clientClass           => "FARM",
     # @ple Label for the client host
     clientLabel           => "uds",
     # @ple The names of the machines to be used for clients.  If not
     #      specified, numClients machines will be reserved.
     clientNames           => undef,
     # @ple use one client machine
     numClients            => 1,
     # @ple Ask rsvpd to randomize its list of available hosts before selecting
     randomizeReservations => 1,
    );

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->reserveHostGroup("client");
  $self->SUPER::set_up();
  $self->getUserMachine();
}

#############################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  map { $_->close() } values(%{$self->{_machines}});
  delete $self->{_machines};
  $self->SUPER::tear_down();
}

#############################################################################
# @inherit
##
sub getTestHosts {
  my ($self) = assertNumArgs(1, @_);
  return ($self->SUPER::getTestHosts(),
          map { ref($_) eq "ARRAY" ? @$_ : () } $self->{clientNames});
}

#############################################################################
# Gets the UserMachine that is used for running tests. If a machine doesn't
# exist then one will be created and cached.
#
# @return a Permabit::UserMachine for the first clientName host
##
sub getUserMachine {
  my ($self) = assertNumArgs(1, @_);
  my $name = $self->{clientNames}[0];
  assertDefined($name);
  $self->{_machines}{$name}
    //= Permabit::RemoteMachine->new(hostname => $name);
  return $self->{_machines}{$name};
}

1;
