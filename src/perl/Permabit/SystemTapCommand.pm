###############################################################################
# Manage a remote systemtap script invocation
#
# To do:
# Make sure errors during execution are recorded somewhere.
# (Should they be fatal?)
# Support stap arguments like -g, -DSTP_NO_OVERLOAD, -DMAXSTRINGLEN=n.
#
# $Id$
##
package Permabit::SystemTapCommand;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp;
use File::Basename;
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertMinArgs
  assertNumArgs
  assertTrue
  assertType
);
use Permabit::Constants;
use Permabit::SystemUtils qw(assertScp);
use Permabit::Utils qw(makeFullPath makeRandomToken retryUntilTimeout);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %PROPERTIES
  = (
     # The Permabit::UserMachine run on
     machine    => undef,
     # The directory to put the script's output file in
     outputDir  => undef,
     # Arguments to pass to the script for @1 / $1 etc.
     scriptArgs => [],
     # The path to the script (already available on the other host)
     script     => undef,
     # String with any stap arguments needed (-DMAXSTRINGLEN=xxx etc).
     stapArgs   => "",
    );

###############################################################################
# Initialize systemtap properties.
##
sub new {
  my ($invocant, %arguments) = assertMinArgs(1, @_);
  my $class = ref($invocant) || $invocant;
  my $self = bless({%PROPERTIES, %arguments}, $class);
  assertDefined($self->{script});
  assertType("Permabit::UserMachine", $self->{machine});

  my $base = basename($self->{script});
  $self->{_id} = makeRandomToken(10);
  $base =~ s/\.stp$//;
  $self->{outputDir} ||= $self->{machine}->{workDir};
  $self->{_outputPath} = makeFullPath($self->{outputDir},
                                      "stap_${base}_$self->{_id}.log");

  # Sometimes the cached version can be incompatible with the kvdo module,
  # resulting in a "build-id mismatch" error in pass 5 (after "starting run").
  # Why systemtap still thinks the cached version is valid is not clear.  We've
  # seen it on SLES11SP3 machines with systemtap 1.5.
  #
  # Currently (10/2015) all our machines are running version 2.6 or 2.7, except
  # SQUEEZE where we do little or no testing and don't care if our SystemTap
  # scripts work.  So it may be safe to re-enable the cache.
  #
  # N.B.: By 2.7 it appears that the backward-compatibility mode for referring
  # to arguments to stap functions written in C (guru mode) is broken.
  $self->{_command} = ("sudo stap -v -F -o $self->{_outputPath} -g"
                       . " --disable-cache $self->{stapArgs}"
                       . " $self->{script} @{$self->{scriptArgs}}");
  return $self;
}

###############################################################################
# Start the tap.
##
sub start {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->{machine};
  my $hostname = $machine->getName();
  if (exists($self->{_pid})) {
    confess("Attempted to start script on $hostname that is already running");
  }
  $machine->executeCommand($self->{_command});
  # The "Pass n: ..." messages go to stderr.
  # stap version 1.5/0.152 on SLES11SP3 prints out an "Updating <path>" line.
  my @outputLines = split('\n', $machine->getStdout());
  @outputLines = grep { !m{^$} } @outputLines;
  @outputLines = grep { !m{Updating /} } @outputLines;
  if (($#outputLines != 0) || ($outputLines[0] !~ /^[0-9]+$/)) {
    confess("couldn't extract stap pid from output");
  }
  my $pid = $outputLines[0];
  $log->debug("stap pid = $pid");
  $self->{_pid} = $pid;
  assertTrue($self->_isRunning());
}

###############################################################################
# Find out whether the systemtap script is still running
#
# @return      whether the systemtap process is still running
##
sub _isRunning {
  my ($self) = assertNumArgs(1, @_);
  return $self->{machine}->sendCommand("sudo kill -0 $self->{_pid}") == 0;
}

###############################################################################
# Stop the systemtap instance.
##
sub stop {
  my ($self) = assertNumArgs(1, @_);
  if (exists($self->{_pid})) {
    $self->{machine}->executeCommand("sudo kill $self->{_pid}");
    retryUntilTimeout(sub { return !$self->_isRunning(); }, $MINUTE);
    delete($self->{_pid});
  }
}

###############################################################################
# Return the output of the systemtap script as a string.
#
# @return the output of the script
##
sub getScriptOutput {
  my ($self) = assertNumArgs(1, @_);
  return $self->{machine}->cat($self->{_outputPath});
}

1;
