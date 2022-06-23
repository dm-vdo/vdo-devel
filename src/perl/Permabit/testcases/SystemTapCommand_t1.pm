##
# Test Permabit::SystemTapCommand
#
# $Id$
##
package testcases::SystemTapCommand_t1;

use strict;
use warnings FATAL => qw(all);
use Cwd qw(cwd);
use English qw(-no_match_vars);

use Permabit::Assertions qw(
  assertLENumeric
  assertNumArgs
  assertRegexpMatches
  assertTrue
);
use Permabit::SystemTapCommand;
use Permabit::UserMachine;
use Permabit::Utils qw(reallySleep);

use base qw(Permabit::Testcase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $SCRIPT = "simple-test.stp";

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The RSVP class clients should be reserved from
     clientClass => "ALBIREO,FARM",
     # @ple The names of the machines to be used
     clientNames => undef,
     # @ple The number of clients that will be used
     numClients  => 1,
    );
##

########################################################################
# @inherit
##
sub listSharedFiles {
  my ($self) = assertNumArgs(1, @_);
  return ($self->SUPER::listSharedFiles(), "src/tools/systemtap");
}

###############################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{_stapProc}) {
    $self->{_stapProc}->stop();
  }
  if (defined($self->{_machine})) {
    $self->{_machine}->close();
  }
  $self->SUPER::tear_down();
}

###############################################################################
# @inherit
##
sub getTestHosts {
  my ($self) = assertNumArgs(1, @_);
  my @hosts = $self->SUPER::getTestHosts();
  if (ref($self->{clientNames}) eq "ARRAY") {
    push(@hosts, @{$self->{clientNames}});
  }
  return @hosts;
}

###############################################################################
##
sub testScript {
  my ($self) = assertNumArgs(1, @_);
  $self->reserveHostGroup("client");
  $self->{_machine}
    = Permabit::UserMachine->new(
                                 hostname    => $self->{clientNames}[0],
                                 nfsShareDir => $self->{nfsShareDir},
                                 scratchDir  => $self->{workDir},
                                 workDir     => $self->{workDir},
                                );
  my %args = (
              machine    => $self->{_machine},
              script     => "$self->{binaryDir}/systemtap/$SCRIPT",
             );
  $self->{_stapProc} = Permabit::SystemTapCommand->new(%args);
  $self->{_stapProc}->start();
  reallySleep(2);
  $self->{_stapProc}->stop();
  my $scriptOutput = $self->{_stapProc}->getScriptOutput();
  assertRegexpMatches(qr/counter = (\d+)/, $scriptOutput);
  $scriptOutput =~ m/counter = (\d+)/;
  assertLENumeric(20, $1, "counter value too low in script output");
  delete $self->{_stapProc};
}

1;
