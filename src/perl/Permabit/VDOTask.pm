##
# This is an AsyncTask that will do an operation on a VDO device.
#
# These tasks fork a separate process, so they should not contain any
# operations that affect the state of test objects, such as changing
# the state of a block device.
#
# $Id$
##
package Permabit::VDOTask;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs assertType);

use base qw(Permabit::AsyncTask);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

###############################################################################
# Record a device being used by the subroutine, so that taskTeardown can close
# the RemoteMachine being used by the device.
#
# @param device  the device.
##
sub useDevice {
  my ($self, $device) = assertNumArgs(2, @_);
  assertEqualNumeric($PID, $self->{parentPid});
  assertType("Permabit::BlockDevice", $device);
  $self->useMachine($device->getMachine());
}

###############################################################################
# Record a filesystem being used by the subroutine, so that taskTeardown can
# close the RemoteMachine being used by the filesystem.
#
# @param fs  the filesystem.
##
sub useFileSystem {
  my ($self, $fs) = assertNumArgs(2, @_);
  assertEqualNumeric($PID, $self->{parentPid});
  assertType("Permabit::FileSystem", $fs);
  $self->useMachine($fs->getMachine());
}

1;
