##
# Base class for tests of memory allocation failures
#
# $Id$
##
package VDOTest::MemoryFailBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $ALLOC_COUNTER           = "/sys/uds/memory/allocation_counter";
my $BYTES_USED              = "/sys/uds/memory/bytes_used";
my $CANCEL_ALLOC_FAILURE    = "/sys/uds/memory/cancel_allocation_failure";
my $ERROR_INJECTION_COUNTER = "/sys/uds/memory/error_injection_counter";
my $LOG_ALLOCATIONS         = "/sys/uds/memory/log_allocations";
my $SCHEDULE_ALLOC_FAILURE  = "/sys/uds/memory/schedule_allocation_failure";
my $TRACK_ALLOCATIONS       = "/sys/uds/memory/track_allocations";

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The machine type, just a cheap local machine since the tests
     #      log a lot.
     clientClass               => "ALBIREO-PMI",
     # @ple The type of VDO device to use, default is managed
     deviceType                => "lvmvdo",
     # @ple set to true to track memory allocations and report leaks
     trackMemory               => 0,
     # @ple Don't verbosely log at shutdown to avoid 100M+ logfiles
     verboseShutdownStatistics => 0,
    );
##

###############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();

  # We will be logging +ENOMEM errors.
  $self->getDevice()->getMachine()->removeKernelLogErrorCheck("mapError");
}

###############################################################################
# Cancel any future memory allocation failure.
##
sub cancelAllocationFailure {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getDevice()->getMachine();
  return $machine->setProcFile("0", $CANCEL_ALLOC_FAILURE);
}

###############################################################################
# Return the number of bytes allocated by the uds module.
#
# @return the number of bytes allocated by the uds module.
##
sub getBytesUsed {
  my ($self) = assertNumArgs(1, @_);
  return $self->getDevice()->getMachine()->catAndChomp($BYTES_USED);
}

###############################################################################
# Determine whether a memory allocation failure has occurred, or is it still
# pending.
#
# @return true if the allocation failure has been scheduled and is yet to
#         occur.
##
sub isAllocationFailurePending {
  my ($self, $count) = assertNumArgs(1, @_);
  my $machine = $self->getDevice()->getMachine();
  return ($machine->catAndChomp($ALLOC_COUNTER)
          < $machine->catAndChomp($ERROR_INJECTION_COUNTER));
}

###############################################################################
# If memory tracking is desired, log the memory blocks that are currently in
# use.
#
# @param track  True to enable memory tracking, or False to turn it off
##
sub logAllocations {
  my ($self, $log) = assertNumArgs(1, @_);
  if ($self->{trackMemory}) {
    my $machine = $self->getDevice()->getMachine();
    $machine->setProcFile("0", $LOG_ALLOCATIONS);
  }
}

###############################################################################
# Schedule a future memory allocation failure
#
# @param count  The number of the future memory allocation that will fail.
#               Thus count-1 allocations will succeed, and then the next
#               one will fail.
##
sub scheduleAllocationFailure {
  my ($self, $count) = assertNumArgs(2, @_);
  my $machine = $self->getDevice()->getMachine();
  return $machine->setProcFile("$count", $SCHEDULE_ALLOC_FAILURE);
}

###############################################################################
# If memory tracking is desired, turn memory tracking on or off.  If it was
# already on, forget all the old allocations and start afresh.
#
# @param track  True to enable memory tracking, or False to turn it off
##
sub trackAllocations {
  my ($self, $track) = assertNumArgs(2, @_);
  if ($self->{trackMemory}) {
    my $machine = $self->getDevice()->getMachine();
    $machine->setProcFile($track ? "1" : "0", $TRACK_ALLOCATIONS);
  }
}

1;
