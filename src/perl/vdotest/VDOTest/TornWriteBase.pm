##
# Test VDO rebuild behavior when the device dies unexpectedly.
#
# This base class provides a test using the Dory device to suddenly stop
# the storage device from doing writes.  It expects the rebuild to succeed,
# and for a vdoAudit to succeed. The Dory device always uses a 512 byte
# block size, and is always set up to tear a few 4K block writes.
#
# $Id$
##
package VDOTest::TornWriteBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest::DoryBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Run Dory with small blocks so we can tear writes
     doryOptions => {
                     blockSize    => 512,
                     cacheBlocks  => 11,
                    },
    );
##

########################################################################
##
sub testAudit {
  my ($self) = assertNumArgs(1, @_);
  foreach my $dedupe ("0", "0.45", "0.9") {
    foreach my $compress ("0", "0.6", "0.9") {
      $self->writeInterruptRecoverAndAudit({tag      => "Audit",
                                            compress => $compress,
                                            dedupe   => $dedupe});
      $self->getDevice()->start();
    }
  }
  $self->_trimInterruptRecoverAndAudit();
}

########################################################################
# Trim data from a VDO device, interrupting the trim by stopping the Dory
# device.  Then recover and audit the VDO device.
#
# @croaks if the VDO recovery fails or the vdoAudit finds a problem.
##
sub _trimInterruptRecoverAndAudit {
  my ($self) = assertNumArgs(1, @_);

  # Trim the device, stopping in time to interrupt the trim.
  # Trim takes about 12 seconds on pfarms.
  my $machine = $self->getDevice()->getMachine();
  $machine->changeKernelLogErrorChecks(del => ["readonly"]);
  my $slice = $self->createSlice();
  my $task = Permabit::VDOTask::SliceOperation->new($slice, "trimEIO");
  $self->getAsyncTasks()->addTask($task);
  $task->start();
  $self->stopDoryDelayed(5);
  $task->result();

  # Try to recover
  $self->recoverAndAuditVDO();
}

1;
