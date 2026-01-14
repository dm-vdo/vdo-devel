##
# Test memory allocation failure while starting a VDO device.
#
# $Id$
##
package VDOTest::MemoryFail01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertFalse assertNumArgs);

use base qw(VDOTest::MemoryFailBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The pass number to start at
     firstPass                 => 1,
     # @ple The pass number to stop at
     lastPass                  => undef,
    );
##

###############################################################################
# Test starting device many times.  Schedule memory allocation failures during
# start.  "Pass N" of the main loop fails the Nth memory allocation.
##
sub testStart {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  $device->stop();
  my $allocationOverhead = $self->getBytesUsed();
  $log->info("Allocation overhead is $allocationOverhead bytes");

  # The memory usage should be stable after a start-stop cycle.
  $device->start();
  $device->stop();
  assertEqualNumeric($allocationOverhead, $self->getBytesUsed(),
                     "Memory leak during start+stop");

  # We will sometimes fail to start the indexer.
  $device->{expectIndexer} = 0;

  # We will sometimes start in read only mode.
  $self->getDevice()->getMachine()->removeKernelLogErrorCheck("readonly");

  # By default this loop runs from pass 1 until we perform the device start
  # without seeing an injected error.  For reproducing a known failure, it is
  # faster to set the firstPass and lastPass parameters.
  for (my $pass = $self->{firstPass};
       !defined($self->{lastPass}) || ($pass <= $self->{lastPass});
       $pass++) {
    $self->logStateInKernLog("Pass $pass");

    # Set up to fail an allocation
    $self->scheduleAllocationFailure($pass);
    $self->trackAllocations(1);

    # Start the VDO device.
    eval {
      $device->start();
    };
    my $startError = $EVAL_ERROR;
    my $vdoMode;
    if (!$startError) {
      # The VDO start succeeded. Maybe starting the index will fail,
      # but if we go into read-only mode, we might not start the index.
      $vdoMode = (split(' ', $device->getStatus()))[4];
      if ($vdoMode ne "read-only") {
        $device->waitForIndex(statusList => [qw(error active)]);
      }
    }

    # We are done when we haven't injected an error, which means that starting
    # the VDO needs fewer allocations than the pass number.
    if ($self->isAllocationFailurePending()) {
      $self->cancelAllocationFailure();
      $self->trackAllocations(0);
      # VDO and the dedupe index should have started.
      assertFalse($startError);
      if ($vdoMode ne "read-only") {
        $device->assertDeduplicationActive();
      }
      last;
    }

    if (!$startError) {
      # VDO started, so we need to stop it.
      $device->stop();
    }

    # There should be no change to the allocated memory.
    if ($allocationOverhead != $self->getBytesUsed()) {
      # The allocated memory did change.  Log the changes to kern.log.
      $self->logAllocations();
    }
    $self->trackAllocations(0);
    assertEqualNumeric($allocationOverhead, $self->getBytesUsed(),
                       "Memory Leak in pass $pass");
  }
}

1;
