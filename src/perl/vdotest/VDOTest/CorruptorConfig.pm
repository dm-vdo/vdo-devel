##
# Basic interface testing of Corruptor.pm device
#
# $Id$
##
package VDOTest::CorruptorConfig;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Basename;
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEq
  assertNe
  assertNumArgs
  assertTrue);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple Use a VDO device
  deviceType => "corruptor",
);
##

#############################################################################
# Test that dmsetup operations status and table return the right info,
# and that dmsetup message at least doesn't crash the machine on an
# unknown message.
##
sub testBasicOps {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $devName = $device->getDeviceName();

  # Test that dmsetup message operations are passed to the device.
  my $kernelLogCursor = $machine->getKernelJournalCursor();
  my $debugMessage = "Mary-Had-A-Little-Lamb";

  eval {
    $device->sendMessage("$debugMessage");
  };

  # Get the table string with resolved device path.
  my $table = $device->makeTableLine();

  # Check table output matches intended config string.
  assertEq($device->getTable(), $table . "\n");

  # Check status output based on intial module state.
  assertEq($device->getStatus(),  $table .
	   " read off random 1 write off random 1\n");

  # Try changing curruption state a few times and make sure the
  # status output matches what we expect.
  $device->enable();
  assertEq($device->getStatus(),  $table .
	   " read on random 1 write on random 1\n");

  $device->disableCurrentRead();
  assertEq($device->getStatus(),  $table .
	   " read off random 1 write on random 1\n");

  $device->enableModuloRead(400);
  assertEq($device->getStatus(),  $table .
	   " read on modulo 400 write on random 1\n");

  $device->enableSequentialWrite(300);
  assertEq($device->getStatus(),  $table .
	   " read on modulo 400 write on sequential 300\n");

  $device->disable();
  assertEq($device->getStatus(),  $table .
	   " read off modulo 400 write off sequential 300\n");

  $device->enableRandomRead(100);
  assertEq($device->getStatus(),  $table .
	   " read on random 100 write off sequential 300\n");

  $device->setModuloRead(400);
  assertEq($device->getStatus(),  $table .
	   " read on modulo 400 write off sequential 300\n");

  $device->setRandomWrite(200);
  assertEq($device->getStatus(),  $table .
	   " read on modulo 400 write off random 200\n");

  $device->enable();
  assertEq($device->getStatus(),  $table .
	   " read on modulo 400 write on random 200\n");

  $device->disable();
  assertEq($device->getStatus(),  $table .
	   " read off modulo 400 write off random 200\n");

  # Handle unknown message.
  eval {
    $device->sendMessage("California");
  };
  assertNe("", $EVAL_ERROR, "unknown message failed to generate an error");

  # Now that some time has passed, finish the dmsetup debug test.
  assertTrue($machine->searchKernelJournalSince($kernelLogCursor,
                                                "$debugMessage"));
}

1;
