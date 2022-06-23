#############################################################################
# Mock Permabit::MegaRaid::UserMachine class, for testing
#
# Used for
#
# $Id$
##
package Permabit::MegaRaid::MockMegaRaid::FakeUserMachine;

use strict;
use warnings FATAL => qw(all);

use Carp qw(confess);
use Permabit::Assertions qw(assertNumArgs);

use base qw(Permabit::UserMachine);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @inherit
#
# Override sendCommand to return fake data to make it so we don't need
# actual device nodes when testing the MegaRaid stuff. More commands
# will need to be added as VirtualDevice::start() changes.
#
# If we don't deem it necessary to override the command, we just pass
# it through to actually run the command on the machine.
#
# @return the same thing as RemoteMachine::sendCommand()
##
sub sendCommand {
  my ($self, $command) = assertNumArgs(2, @_);
  if ($command =~ qr/^readlink -f \/dev\/disk/) {
    return $self->_fakeReadlink($command);
  } elsif ($command =~ qr/^ls -Hl \/dev\/disk/) {
    return $self->_fakeLs($command);
  } elsif ($command =~ qr/chmod .* \/dev\/disk/) {
    return $self->_fakeIPCsend($command, '', '', 0);
  } else {
    $self->{_fakeStdout} = undef;
    $self->{_fakeStderr} = undef;
    return $self->SUPER::sendCommand($command);
  }
}

#############################################################################
# Runs a fake `readlink' command on the machine to fake out when BlockDevice
# tries to find the "real" location of the device node.
#
# @param command     The command line that was run
#
# @return the same thing as IPC::Session::send()
##
sub _fakeReadlink {
  my ($self, $command) = assertNumArgs(2, @_);
  if ($command !~ /^readlink -f (\S+)/) {
    confess("bad command syntax: $command");
  }
  # Just return the original name (as if there was no link to resolve)
  return $self->_fakeIPCsend($command, $1, '', 0);
}

#############################################################################
# Runs a fake `ls' command on the machine to fake out when BlockDevice
# tries to figure out the major/minor number of a device.
#
# @param command     The command line that was run
#
# @return the same thing as IPC::Session::send()
##
sub _fakeLs {
  my ($self, $command) = assertNumArgs(2, @_);
  if ($command !~ /^ls -Hl (\S+)/) {
    confess("bad command syntax: $command");
  }
  # Made up data, should be harmless
  return $self->_fakeIPCsend($command,
                             "brw-rw-rw- 1 root disk 8, 0 Apr  4 12:46 $1",
                             '',
                             0);
}

#############################################################################
# Sets the state of the IPC::Session to be as if we actually ran the command
# and returns the same results as IPC::Session::send()
#
# @param command     The command to fake out
# @param stdout      The fake output of the command
# @param stderr      The fake stderr of the command
# @param errno       The exit status of the command
#
# @return the same thing as IPC::Session::send()
##
sub _fakeIPCsend {
  my ($self, $command, $stdout, $stderr, $errno) = assertNumArgs(5, @_);
  $log->debug("faking out command: $command");
  $self->{_fakeStdout} = $stdout;
  $self->{_fakeStderr} = $stderr;

  return $errno unless wantarray;
  return (errno  => $errno,
	  stdout => $stdout,
	  stderr => $stderr);
}

#############################################################################
# @inherit
##
sub getStdout {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_fakeStdout} // $self->SUPER::getStdout();
}

#############################################################################
# @inherit
##
sub getStderr {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_fakeStderr} // $self->SUPER::getStderr();
}

1;
