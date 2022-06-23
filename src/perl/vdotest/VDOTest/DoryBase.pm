##
# Base class for tests that use the Dory device.
#
# The Dory device supports a stopWriting() method that causes future write
# requests to fail.  There is little sense to using a Dory device unless this
# stopWriting() method is called.
#
# This base class provide these services:
#
# -- The setup code that sets up a VDO device on top of a Dory device.
#
# -- A recoverAndAuditVDO method that will recover VDO and audit the VDO after
#    a Dory device has been stopped.
#
# -- A stopDoryDelayed method that will use an AsyncSub to stop the Dory device
#    after a time delay.
#
# $Id$
##
package VDOTest::DoryBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEqualNumeric
  assertNumArgs
  assertRegexpMatches
);
use Permabit::Constants;
use Permabit::Utils qw(reallySleep);
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a vdo-dory stack.
     deviceType        => "vdo-dory",
     # @ple Dory device options.
     doryOptions       => {},
     # @ple turn on compression
     enableCompression => 1,
    );
##

########################################################################
# Recover from stopping a dory device.  Shut down the VDO device and the dory
# device.  Then restart the dory device and the VDO device.
#
# @croaks if the VDO recovery fails
##
sub recoverVDO {
  my ($self) = assertNumArgs(1, @_);
  my $doryDevice = $self->getDoryDevice();
  my $vdoDevice  = $self->getDevice();

  my $fs;
  if ($self->{useFilesystem}) {
    $log->info("Stop Filesystem");
    $fs = $self->getFileSystem();
    $fs->stop();
  }

  $log->info("Stop VDO");
  $vdoDevice->stop();
  $vdoDevice->dumpMetadata("before");
  $log->info("Stop DORY");
  $doryDevice->stop();
  $log->info("Start DORY");
  $doryDevice->start();

  eval {
    $log->info("Recover VDO");
    $vdoDevice->recover();
    if ($self->{useFilesystem}) {
      $log->info("Mount Filesystem");
      $fs->mount();
    }
  };
  if (my $ee = $EVAL_ERROR) {
    $self->_recoveryFailure("recovery");
    die($ee);
  }
}

########################################################################
# Recover from stopping a dory device.  Shut down the VDO device and the dory
# device.  Then restart the dory device and the VDO device.  Finally shutdown
# and audit the VDO device.
#
# @croaks if the VDO recovery fails or the vdoAudit finds a problem.
##
sub recoverAndAuditVDO {
  my ($self) = assertNumArgs(1, @_);
  my $vdoDevice  = $self->getDevice();

  $self->recoverVDO();

  eval {
    $log->info("Stop and Audit VDO");
    if ($self->{useFilesystem}) {
      $self->getFileSystem()->stop();
    }
    $vdoDevice->waitUntilRecoveryComplete();
    $vdoDevice->stop();
    $vdoDevice->getMachine()->changeKernelLogErrorChecks(add => ["readonly"]);
  };
  if (my $ee = $EVAL_ERROR) {
    $self->_recoveryFailure("recovery");
    die($ee);
  }

  my $auditResult = $vdoDevice->doVDOAudit();
  if ($auditResult->{returnValue} != 0) {
    $self->_recoveryFailure("audit");
  }
  assertEqualNumeric($auditResult->{returnValue}, 0);
  assertRegexpMatches(qr/All pbn references matched./, $auditResult->{stderr});
}

########################################################################
# Stop the Dory device after a time delay
#
# @param delay  Time delay in seconds
##
sub stopDoryDelayed {
  my ($self, $delay) = assertNumArgs(2, @_);
  reallySleep($delay);
  $self->getDoryDevice()->stopWriting();
}

########################################################################
# Write data to VDO, interrupting the write by stopping the Dory device.  Then
# recover and audit the VDO device.
#
# @param writeEIOArgs  A hashref to the arguments for writing a slice using
#                      Permabit::GenSlice::writeEIO.
#
# @croaks if the VDO recovery fails or the vdoAudit finds a problem.
##
sub writeInterruptRecoverAndAudit {
  my ($self, $writeEIOArgs) = assertNumArgs(2, @_);

  my $machine = $self->getDevice()->getMachine();
  $machine->changeKernelLogErrorChecks(del => ["readonly"]);

  my $slice;
  my $task;
  if ($self->{useFilesystem}) {
    $slice = $self->createSlice(fs => $self->getFileSystem(),
                                numBytes => 3 * $GB,
                                numFiles => 500);
    $task = Permabit::VDOTask::SliceOperation->new($slice, "writeEIO",
                                                   direct   => 1,
                                                   fsync    => 1,
                                                   %{$writeEIOArgs});
  } else {
    $slice = $self->createSlice();
    $task = Permabit::VDOTask::SliceOperation->new($slice, "writeEIO",
                                                   direct => 1,
                                                   %{$writeEIOArgs});
  }
  $self->getAsyncTasks()->addTask($task);
  $task->start();
  $self->stopDoryDelayed(30);
  $task->result();

  # Try to recover
  $self->recoverAndAuditVDO();
}

########################################################################
# Process a failure in recoverAndAuditVDO.
#
# @param label  Type of failure
##
sub _recoveryFailure {
  my ($self, $label) = assertNumArgs(2, @_);
  my $host    = $self->getDevice()->getMachine()->getName();
  $self->manualWaitPoint("doryFailure",
                         "Dory test failed on host $host"
                         . " with failed $label");
  $log->info(ucfirst($label) . " failed, so dump the metadata");
  $self->getDevice()->dumpMetadata("after");
}

1;
