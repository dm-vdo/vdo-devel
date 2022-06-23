##
# Test memory allocation failure while running a command that modifies the VDO
# device.
#
# $Id$
##
package VDOTest::MemoryFail02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEqualNumeric
  assertFalse
  assertNumArgs
  assertTrue
);
use Permabit::Constants;

use base qw(VDOTest::MemoryFailBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The pass number to start at
     firstPass          => 1,
     # @ple The pass number to stop at
     lastPass           => undef,
     # @ple The bigger logical size to grow to
     logicalBiggerSize  => 10 * $GB,
     # @ple Initial logical size
     logicalSize        => 5 * $GB,
     # @ple The bigger physical size to grow to
     physicalBiggerSize => 10 * $GB,
     # @ple Initial physical size
     physicalSize       => 5 * $GB,
     # @ple the number of bits in the VDO slab
     slabBits           => $SLAB_BITS_TINY,
    );
##

###############################################################################
# Test growLogical many times.  Schedule memory allocation failures during
# growth.  "Pass N" of the main loop fails the Nth memory allocation.
##
sub testGrowLogical {
  my ($self) = assertNumArgs(1, @_);
  $self->_runModificationTest(\&_growLogical);
}

###############################################################################
# The evalModifyDevice routine for testing grow logical.
#
# @return the error returned from the grow logical operation
##
sub _growLogical {
  my ($self) = assertNumArgs(1, @_);
  eval { $self->getDevice()->growLogical($self->{logicalBiggerSize}); };
  return $EVAL_ERROR;
}

###############################################################################
# Test growPhysical many times.  Schedule memory allocation failures during
# growth.  "Pass N" of the main loop fails the Nth memory allocation.
##
sub testGrowPhysical {
  my ($self) = assertNumArgs(1, @_);
  $self->_runModificationTest(\&_growPhysical);
}

###############################################################################
# The evalModifyDevice routine for testing grow physical.
#
# @return the error returned from the grow physical operation
##
sub _growPhysical {
  my ($self) = assertNumArgs(1, @_);
  eval { $self->getDevice()->growPhysical($self->{physicalBiggerSize}); };
  return $EVAL_ERROR;
}

###############################################################################
##
sub _runModificationTest {
  my ($self, $evalModifyDevice) = assertNumArgs(2, @_);
  my $device = $self->getDevice();

  # By default this loop runs from pass 1 until we perform the modification
  # without seeing an injected error.  For reproducing a known failure, it is
  # faster to set the firstPass and lastPass parameters.
  for (my $pass = $self->{firstPass};
       !defined($self->{lastPass}) || ($pass <= $self->{lastPass});
       $pass++) {
    $self->logStateInKernLog("Pass $pass");

    # Record the current number of bytes used, and set up to fail an
    # allocation.
    my $allocationOverhead = $self->getBytesUsed();
    $self->scheduleAllocationFailure($pass);
    $self->trackAllocations(1);

    # Modify the VDO device.
    my $modifyError = $evalModifyDevice->($self);

    # We are done when we haven't injected an error, which means that modifying
    # the VDO needs fewer allocations than the pass number.
    if ($self->isAllocationFailurePending()) {
      $self->cancelAllocationFailure();
      $self->trackAllocations(0);
      # The modification should have succeeded.
      assertFalse($modifyError);
      last;
    }

    # Grow should have failed.
    assertTrue($modifyError);

    # There should be no change to the allocated memory.
    if ($allocationOverhead != $self->getBytesUsed()) {
      # The allocated memory did change.  Log the changes to kern.log.
      $self->logAllocations();
    }
    $self->trackAllocations(0);
    assertEqualNumeric($allocationOverhead, $self->getBytesUsed(),
                       "Memory Leak in pass $pass");

    # Sometimes the device is left in a bad state, and stopping the device
    # will notice a problem.
    $device->stop();
    $device->start();
  }
}

1;
