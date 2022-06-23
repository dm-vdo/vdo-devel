######################################################################
# A generic runner for a long-running server process such as the
# Albireo Indexer service.
#
# $Id$
##
package Permabit::ProcessServer;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp;
use Log::Log4perl;
use Storable qw(dclone);
use Time::HiRes qw(usleep);

use Permabit::Assertions qw(assertDefined assertNumArgs assertMinMaxArgs);
use Permabit::Constants;
use Permabit::Utils qw(makeFullPath retryUntilTimeout);

use base qw(Permabit::RemoteMachine);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $indexerCounter = 0;

my %PROPERTIES
  = (
     # @ple The command to run
     command      => undef,
     # @ple The stdout/stderr file
     output       => undef,
     # The ouput of the last pgrep, if any
     _pgrepOutput => undef,
     # @ple Command to look for (default the same as command)
     psCommand    => undef,
     # @ple The directory to run the service from. This should be a directory
     #      that you have write privileges since the command output gets saved
     #      here.
     runDir       => "/",
     # @ple Whether the process is running:
     #      undef if it was never started
     #      1     start has completed successfully
     #      0     stop has completed successfully
     _running     => undef,
     # @ple The service name, used for log messages
     serviceName  => "Background Process",
     # @ple The process ID
     _serverPid   => undef,
    );

######################################################################
# Instantiates a new ProcessServer.
##
sub new {
  my $invocant = shift(@_);
  my $class = ref($invocant) || $invocant;
  my %otherArgs = @_;

  $log->debug("Creating instance of " . __PACKAGE__);
  my $self = $class->SUPER::new(%{ dclone(\%PROPERTIES) },
                                output => "indexer$PID-$indexerCounter.out",
                                %otherArgs,
                               );

  assertDefined($self->{command}, "Command not set");
  $self->{psCommand} //= $self->{command};
  $self->{psCommand} =~ s/^\s+|\s+$//; # Trim whitespace so our pgrep
  $self->{command}   =~ s/^\s+|\s+$//; # commands work as expected.
  $indexerCounter++;
  return $self;
}

######################################################################
# @inherit
##
sub start {
  my ($self) = assertNumArgs(1, @_);
  if (!defined($self->{_running}) || $self->{_running} == 0) {
    $log->debug("Starting " . $self->{serviceName} . " on $self->{hostname}");
    $self->_runBackgroundCmd("cd $self->{runDir} && $self->{command}");
    eval {
      $self->{_serverPid}
        = retryUntilTimeout(sub { $self->getPid() },
                            "Couldn't get PID for $self->{serviceName}",
                            20, 5);
    };
    if (my $error = $EVAL_ERROR) {
      my $stdout = $self->getStdout();
      $self->logProcesses();
      $self->sendCommand('cat ' . $self->getOutput());
      my $output = $self->getStdout();
      croak("Couldn't start '$self->{command}', got: $stdout,"
            . " got: [$output]\n$error");
    }
  }
  $self->{_running} = 1;
  return;
}

######################################################################
# Runs a command in the background and directs its stdout and stderr
# to a file. To get the output, the user can cat the file returned by
# the #getOutput method.
#
# @param command  The command to run in the background.
#
# @croaks if the command cannot be spawned.
##
sub _runBackgroundCmd {
  my ($self, $command) = assertNumArgs(2, @_);
  $self->runSystemCmd("$command > " . $self->getOutput()
                      . " 2>&1 <&- &");
}

######################################################################
# stop the process server
#
# @oparam signal  The signal option (including dash) to use when killing the
#                   process
#
# @return 1 if the process was successfully stopped
#         2 if it was already stopped
#
# @croaks if there is an error in stopping the process.
##
sub stop {
  my ($self, $signal) = assertMinMaxArgs([''], 1, 2, @_);
  if (!defined($self->{_running}) || ($self->{_running} == 0)) {
    return 2;
  }
  # Convert the command into an extended regular expression (see the regex(7)
  # man page) by quoting metacharacters in the command string.
  my $psCommand = $self->{psCommand};
  $psCommand =~ s/([+*.\\])/\\$1/g;
  my $stopCheck = sub {
    return $self->sendCommand("pgrep -f '$psCommand'") == 1;
  };
  my $stopMessage = "Stopping $self->{serviceName} on $self->{hostname}";
  $log->debug($stopMessage);
  $self->sendCommand("sudo pkill -CONT -f '$psCommand'");
  $self->sendCommand("sudo pkill -f '$psCommand'");
  retryUntilTimeout($stopCheck, "$stopMessage failed", $MINUTE, 2);
  $self->close();
  $self->{_serverPid} = undef;
  $self->{_running} = 0;
  return 1;
}

######################################################################
# signal the process server with a non-fatal signal
#
# @oparam signal  The signal option (including dash) to use when killing the
#                 process
#
# @croaks if the process can't be signaled
##
sub signal {
  my ($self, $signal) = assertMinMaxArgs([''], 1, 2, @_);
  if (defined($self->{_running}) && ($self->{_running} == 1)
      && defined($self->{_serverPid})) {
    $log->debug("Signalling $self->{serviceName} on $self->{hostname}");
    # use sudo b/c the given command might have used sudo and what's the harm
    # if it didn't?
    my $cmd = "sudo kill $signal $self->{_serverPid}";
    $log->debug($cmd);
    $self->sendCommand($cmd);
  }
}

######################################################################
# Check for the process running, leaving the results of pgrep in
# the _pgrepOutput property for use by the caller.
#
# @oparam verbose log verbosely
#
# @return       true iff the process is running
#
# @croaks if the pgrep command fails
##
sub _checkProcess {
  my ($self, $verbose) = assertMinMaxArgs([1], 1, 2, @_);

  my $tmp = $self->{psCommand};
  $tmp =~ s/([+*.\\])/\\$1/g;
  my $cmd = "pgrep -f '" . $tmp . "'";
  if ($verbose) {
    $log->debug("command is $cmd");
  }
  my $errno;
  my $retries = 5;
  while (!defined($errno)) {
    eval {
      $errno = $self->sendCommand($cmd);
      $self->{_pgrepOutput} = $self->getStdout();
      if ($verbose) {
        $log->debug("errno is $errno");
        $log->debug("output is $self->{_pgrepOutput}");
      }
      if ($errno != 0 && $errno != 1) {
        $errno = undef;
        confess("pgrep failed");
      }
    };
    if ($EVAL_ERROR) {
      if (--$retries > 0) {
        $log->warn("Will retry $cmd due to failure: $EVAL_ERROR");
        next;
      } else {
        die($EVAL_ERROR);
      }
    }
  }
  return ($errno == 0);
}

######################################################################
# Returns the process ID of the server command; 0 if it isn't running.
#
# @return the process ID
#
# @croaks if checking for the process fails.
##
sub getPid {
  my ($self) = assertNumArgs(1, @_);
  return $self->_getPidWithRetries(10);
}

######################################################################
# A helper function for getPid that will try multiple times to
# determine the pid if we keep getting multiple pids back from pgrep.
# This is useful for programs like valgrind that briefly fork/exec a
# child process that will temporarily have the same command listing
# in the process table.
#
# @param  retries      The number of times to retry getting the pid.
#
# @return the process ID
#
# @croaks if checking for the process fails and there are no retries
#         left.
##
sub _getPidWithRetries {
  my ($self, $retries) = assertNumArgs(2, @_);
  # If we stopped cleanly, don't bother doing the process check
  if ((defined($self->{_running}) && $self->{_running} == 0)
      || !$self->_checkProcess()) {
    return 0;
  }

  # Get any PIDs found by _checkProcess
  my $stdout = $self->{_pgrepOutput};
  my @pids = $stdout =~ /^(\d+)$/mg;
  if (scalar(@pids) == 1) {
    return $pids[0];
  } elsif ((scalar(@pids) > 1) && ($retries > 0)) {
    $log->debug("Found multiple pids -- sleeping and retrying: @pids");
    usleep(200 * 1000);   # sleep 0.2 seconds = 200,000 microseconds
    return $self->_getPidWithRetries(--$retries);
  }
  confess("pgrep output not as expected: $stdout");
}

######################################################################
# Return the file with stdout/stderr.
#
# @return the local path to the file
##
sub getOutput {
  my ($self) = assertNumArgs(1, @_);
  return makeFullPath($self->{runDir}, $self->{output});
}

1;
