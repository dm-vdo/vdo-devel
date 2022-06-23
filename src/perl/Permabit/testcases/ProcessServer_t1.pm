##
# Test of Permabit::ProcessServer object
#
# $Id$
##
package testcases::ProcessServer_t1;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEqualNumeric
  assertEvalErrorMatches
  assertMinArgs
  assertNENumeric
  assertNumArgs
);

use base qw(Permabit::Testcase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);
my @_servers;

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The RSVP class clients should be reserved from
     clientClass => undef,
     # @ple The names of the machines to be used
     clientNames => undef,
     # @ple The number of clients that will be used
     numClients  => 1,
    );
##

######################################################################
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();
  undef(@_servers);
  $self->reserveHostGroup("client");
  $self->{_host} = $self->{clientNames}[0];
}

######################################################################
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  foreach my $ps (@_servers) {
    $log->debug("tearing down $ps");
    eval { $ps->tearDown() };
    if ($EVAL_ERROR) {
      $log->error("failed to stop Process Server: $EVAL_ERROR");
    }
  }
  $self->SUPER::tear_down();
}

#############################################################################
# @inherit
##
sub getTestHosts {
  my ($self) = assertNumArgs(1, @_);
  my @hosts = $self->SUPER::getTestHosts();
  if (ref($self->{clientNames}) eq "ARRAY") {
    @hosts = (@hosts, @{$self->{clientNames}});
  }
  return @hosts;
}

######################################################################
##
sub makeProcessServer {
  my ($self, $cmd, @args) = assertMinArgs(2, @_);
  my $ps = testcases::ProcessServer->new(command  => $cmd,
                                         hostname => $self->{_host},
                                         runDir   => $self->{nfsShareDir},
                                         @args);
  push(@_servers, $ps);
  return $ps;
}

######################################################################
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  # Launch a command that won't stop on its own so the object
  # will have a chance to kill it.
  my $ps = $self->makeProcessServer('tail -f /etc/hosts');
  assertEqualNumeric(0, $ps->getPid());
  $ps->start();
  assertNENumeric(0, $ps->getPid(), "Couldn't determine pid of 'cat'");
  $ps->stop();
  assertEqualNumeric(0, $ps->getPid());
}

######################################################################
##
sub testFailedStart {
  my ($self) = assertNumArgs(1, @_);
  my $ps = $self->makeProcessServer('false');
  eval { $ps->start() };
  assertEvalErrorMatches(qr/Couldn't start 'false'/);
}

######################################################################
##
sub testMultiPids {
  my ($self) = assertNumArgs(1, @_);
  # Launching the first command should succeed.
  my $ps = $self->makeProcessServer('tail -f /etc/hosts');
  $ps->start();
  assertEqualNumeric(0, $ps->{_retries}, "$ps retried too many times");

  # Make another command that will have a similar command line, and
  # simulate an application that's taking a long time to do an exec
  # after the fork.
  my $ps2 = $self->makeProcessServer('tail -f /etc/host.conf');
  $ps2->start();
  {
    local $ps2->{psCommand} = 'tail -f /etc/host';
    eval { $ps2->getPid() };
  }
  assertEvalErrorMatches(qr/pgrep output not as expected/);
  assertEqualNumeric(10, $ps2->{_retries}, "$ps2 retried too many times");
}

######################################################################
##
sub testSignals {
  my ($self) = assertNumArgs(1, @_);
  my $cmd = q|sudo perl -we 'my $c = 1;
                             $SIG{USR1} = sub {print $c++ . " "};
                             $SIG{TERM} = sub {exit 0};
                             while (1) {sleep 30}
                             exit 1;'|;

  # Trying to match the whole command line is a nightmare to escape
  # all the special characters so just punt and match the first bit.
  my $ps = $self->makeProcessServer($cmd, psCommand => '^perl -we');
  $ps->start();
  $ps->signal('-USR1');
  $ps->signal('-USR1');
  $ps->signal('-USR1');
  $ps->signal(); # Test signal with no args.
  $ps->stop();
  $ps->openSession();
  $ps->sendCommand('cat ' . $ps->getOutput());
  $log->debug("output: " . $ps->getStdout());
  $self->assert_matches(qr/^1 2 3 $/, $ps->getStdout());
}

######################################################################
######################################################################
package testcases::ProcessServer;
use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use base qw(Permabit::ProcessServer);

$log = Log::Log4perl->get_logger('TestProcessServer');

######################################################################
# Keep track of the number of retries we did.
##
sub _getPidWithRetries {
  my $self = shift;
  $self->{_retries}++;
  $log->debug("retries = $self->{_retries}");
  return $self->SUPER::_getPidWithRetries(@_);
}

######################################################################
# We need to decrement the retries count every time we call getPid
# because that's not the kind of retries we are trying to measure.
##
sub getPid {
  my $self = shift;
  $self->{_retries}--;
  return $self->SUPER::getPid(@_);
}

######################################################################
# Be more forceful in cleaning up after our test ProcessServer so
# we don't leak machines. There's a subtle issue where real ProcessServer
# objects won't try to cleanup the process it created if the getPid
# call fails during start. That might be a real bug with ProcessServer
# but then again, this test does use them a little abnormally.
##
sub tearDown {
  my $self = shift;
  my $tmp = $self->{psCommand};
  $tmp =~ s/([+*.\\])/\\$1/g;
  eval {
    my $errno = $self->sendCommand("pkill -KILL -f '$tmp'");
    if ($errno != 0 && $errno != 1) {
      $log->warn("pkill failed");
    }
  };
  if ($EVAL_ERROR) {
    $log->error("$self tearDown croaked: $EVAL_ERROR");
  }
  return $self->SUPER::tearDown(@_);
}

1;
