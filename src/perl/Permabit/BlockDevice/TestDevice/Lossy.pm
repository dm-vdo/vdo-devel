##
# Perl object that represents a block device with a short term memory problem.
#
# $Id$
##
package Permabit::BlockDevice::TestDevice::Lossy;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(Permabit::BlockDevice::TestDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple block size
     blockSize   => 4 * $KB,
     # @ple The number of cache blocks
     cacheBlocks => 0,
     # @ple mask that can be used to cause this device to tear writes
     tornMask    => 0,
     # @ple modulus that can be used to cause this device to tear writes
     tornModulus => 0,
    );
##

########################################################################
# @inherit
##
sub makeTableLine {
  my ($self) = assertNumArgs(1, @_);
  return join(' ',
              $self->SUPER::makeTableLine(),
              $self->{blockSize},
              $self->{cacheBlocks},
              $self->{tornMask},
              $self->{tornModulus});
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::activate();
  $self->addPreDeactivationStep(sub { $self->collectStats(); });
}

########################################################################
# Colect and log stats when before deactivation.
##
sub collectStats {
  my ($self) = assertNumArgs(1, @_);
  $self->logMessageData("statistics");
  $self->logMessageData("state");
  $self->logMessageData("show_cache");
}

########################################################################
# Stop the device performing any writes.
##
sub stopWriting {
  my ($self) = assertNumArgs(1, @_);
  $self->sendMessage("stop");
}

########################################################################
# Collect the output of a message
#
# @param message  The message to send
##
sub logMessageData {
  my ($self, $message) = assertNumArgs(2, @_);
  my $result = $self->sendMessage($message);
  $log->info($result);
}

########################################################################
# Send a message to the lossy device.
#
# @param message  The message to send to the device
##
sub sendMessage {
  my ($self, $message) = assertNumArgs(2, @_);
  my $device = $self-> getDeviceName();
  return $self->runOnHost("sudo dmsetup message $device 0 $message");
}

1;
