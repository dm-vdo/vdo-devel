######################################################################
# Spawn a blktrace process
#
# $Id$
##
package Permabit::BlkTrace;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp qw(confess);
use Log::Log4perl;

use Permabit::Assertions qw(assertDefined assertMinMaxArgs assertNumArgs);
use Permabit::ProcessServer;
use Permabit::SupportUtils qw(convertToFormatted);
use Permabit::SystemUtils qw(runCommand);

use base qw(Permabit::GenericCommand);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %PROPERTIES =
  (
   # @ple core file name of trace file; used to generate the base file name
   #      by appending the current epoch
   coreFileName   => undef,
   # the binary to use
   binary         => "blktrace",
   # @ple fully-resolved pathname of the device
   devicePath     => undef,
   # @ple filter of events to trace
   filter         => undef,
   # Executable name
   name           => "blktrace",
   # @ple base file name of trace file
   _baseFileName  => undef,
   # A reference to the running blktrace process
   _processServer => undef,
  );

######################################################################
# Class method that determines if blktrace is being run on the specified
# host against the specified device.
#
# @param  host      name of host to check
# @param  path      fully-resolved path to device on host
#
# @return boolean   true => blktrace is being run against the device
##
sub isDeviceTraced {
  my ($class, $host, $path) = assertNumArgs(3, @_);

  my $pattern = "^blktrace.* -d $path";
  my $cmd = "pgrep -f '" . $pattern . "'";
  my $result = runCommand($host, $cmd);
  if (($result->{status} != 0) && ($result->{status} != 1)) {
    confess("pgrep failed");
  }
  return $result->{status} == 0;
}

#############################################################################
# @inherit
##
sub getProperties {
  my $self = shift;
  return ( $self->SUPER::getProperties(),
           %PROPERTIES );
}

######################################################################
# @inherit
##
sub run {
  my ($self) = assertNumArgs(1, @_);
  # Override the base run() because it isn't really meaningful.
  $self->start();
}

######################################################################
# @inherit
##
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  my @args;

  $self->_addValueOption(\@args, '_baseFileName', '-o');
  $self->_addValueOption(\@args, 'devicePath', '-d');
  $self->_addValueOption(\@args, 'filter', '-a');
  return @args;
}

######################################################################
# @inherit
##
sub new {
  my ($invocant, $parent, $params) = assertNumArgs(3, @_);
  my $class = ref($invocant) || $invocant;
  my $self = $class->SUPER::new($parent, $params);
  assertDefined($self->{devicePath});
  if (!defined($self->{coreFileName})) {
    my @pathComponents = split('/', $self->{devicePath});
    $self->{coreFileName} = $pathComponents[-1];
  }
  my $date = convertToFormatted(time(), 1);
  $date =~ s/[\/:\s]/_/g;
  $self->{_baseFileName} = join(".", $self->{coreFileName}, $date);
  return $self;
}

######################################################################
# Create a properly configured ProcessServer.
##
sub buildServer {
  my ($self) = assertNumArgs(1, @_);
  # Need to specify the psCommand because the full command cd's into a
  # directory, sets ulimt, etc...
  my $psCommand = join(' ', "^$self->{binary}", $self->getArguments());
  return Permabit::ProcessServer->new(command       => $self->buildCommand(),
                                      psCommand     => $psCommand,
                                      hostname      => $self->{host},
                                      serviceName   => 'Block Trace',
                                      runDir        => $self->{runDir},
                                      logfileRegexp => qr/\.log/,
                                      logDirectory  => $self->{runDir},
                                     );
}

######################################################################
# Start blktrace.
##
sub start {
  my ($self) = assertNumArgs(1, @_);
  my $ps = $self->buildServer();
  eval {
    $self->{_processServer} = $ps;
    $ps->start();
  };
  if (my $error = $EVAL_ERROR) {
    $self->{_processServer}->logProcesses();
    my $result = runCommand($self->{host}, "cat " . $ps->getOutput());
    confess($error . ', stdout: [' . $result->{stdout} . '], stderr: ['
            . $result->{stderr} . ']');
  }
}

######################################################################
# Send a signal to blktrace
#
# @oparam signal  The signal option (including dash) to use
##
sub _signal {
  my ($self, $signal) = assertNumArgs(2, @_);
  $self->{_processServer}->signal($signal);
}

######################################################################
# Pause blktrace
##
sub pause {
  my ($self) = assertNumArgs(1, @_);
  $self->_signal("-STOP");
}

######################################################################
# Continue blktrace
##
sub continue {
  my ($self) = assertNumArgs(1, @_);
  $self->_signal("-CONT");
}

######################################################################
# Stop blktrace
#
# @oparam signal    The signal option (including dash) to use when killing the
#                   process
#
# @return 1 if there is no Permabit::ProcessServer, otherwise it returns
#         the value of #Permabit::ProcessServer::stop.
#
# @croaks if we can't stop the node.
##
sub stop {
  my ($self, $signal) = assertMinMaxArgs([''], 1, 2, @_);
  if ($self->{_processServer}) {
    return $self->{_processServer}->stop($signal);
  }
  return 1;
}

######################################################################
# Wait until blktrace has completely exited.
##
sub waitUntilDead {
  my ($self, $index) = assertNumArgs(2, @_);
  while ($self->{_processServer}->getPid() != 0) {
    sleep(2);
  }
}

######################################################################
# kill the process server with SIGKILL
##
sub forceKill {
  my ($self) = assertNumArgs(1, @_);
  $self->stop("-KILL");
}

######################################################################
# Forcibly dismantle a node
##
sub tearDown {
  my ($self) = assertNumArgs(1, @_);
  eval { $self->stop() };
  if ($EVAL_ERROR) {
    $log->error("stop failed in tearDown: $EVAL_ERROR");
  }

  if ($self->{_processServer}) {
    $self->{_processServer}->tearDown();
  }
}

#############################################################################
# Returns the base file name.
#
# @return the base file name
##
sub getBaseFileName {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_baseFileName};
}

#############################################################################
# Returns the core file name.
#
# @return the core file name
##
sub getCoreFileName {
  my ($self) = assertNumArgs(1, @_);
  return $self->{coreFileName};
}

######################################################################
# Return debugging information on the network state.
#
# @return string containing what commands were run and what the results were.
##
sub getNetworkState {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{_processServer}) {
    return $self->{_processServer}->getNetworkState();
  }
  return "$self has no network state";
}

##########################################################################
# Utility to save all logfiles returned by getLogFileList() by copying
# them from the remote machine to a local directory.
#
# @param logDir The directory to save log files in.
##
sub saveLogFiles {
  my ($self, $logDir) = assertNumArgs(2, @_);
  if ($self->{_processServer}) {
    $self->{_processServer}->saveLogFiles($logDir);
  }
}

1;
