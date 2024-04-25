##############################################################################
# A command for running the dmtest command
#
# $Id: $
##
package Permabit::CommandString::DMTest;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Utils qw(makeFullPath);

use base qw(Permabit::CommandString);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %COMMANDSTRING_PROPERTIES
  = (
     # the command to run, one of list, run, or log
     command             => undef,
     # the storage device for the VDO volume
     device              => undef,
     # the directory to run the dmtest shell out of
     dmtestDir           => undef,
     # the test glob to work on
     dmtestName          => undef,
     # if specified, the path to use for a log file
     logfile             => undef,
     # executable name
     name                => "dmtest",
     # Print commands instead of executing them
     noRun               => undef,
    );

###############################################################################
# Build the start of every command.  Make sure to cd into dmtest so shell
# script will work.
# @return  A list of command string fragments; should not be empty.
##
sub getBaseCommand {
  my ($self) = assertNumArgs(1, @_);
  my @cmd = ("cd", $self->{dmtestDir}, "&& umask 0 && ulimit -c unlimited &&");
  return @cmd;
}


###############################################################################
# @inherit
##
sub getEnvironment {
  my ($self) = assertNumArgs(1, @_);
  $self->{env}->{PYTHONDONTWRITEBYTECODE} = "true";
  if (defined($self->{dmtestDir})) {
    $self->{env}->{PYTHONPATH} = $self->{dmtestDir} . ":\$PYTHONPATH";
  }
  $self->{env}->{DMTEST_RESULT_SET} = "vdo";

  return $self->SUPER::getEnvironment();
}

###############################################################################
# @inherit
##
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  my @SPECIFIERS = qw(
    command
  );

  my $command = $self->{command};
  if (($command eq "list")
      || ($command eq "run")
      || ($command eq "log")) {
    push(@SPECIFIERS, qw(dmtestName=--rx));
  };

  return $self->SUPER::getArguments(@SPECIFIERS);
}

1;
