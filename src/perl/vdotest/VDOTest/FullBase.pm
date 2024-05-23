##
# Base class for testing behaviour when VDO runs out of physical space
#
# $Id$
##
package VDOTest::FullBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::Utils qw(parseBytes);
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDO device
     deviceType        => "lvmvdo",
     # @ple Use VDO compression
     enableCompression => 1,
     # @ple use a large enough logical size so that VDO will report ENOSPC
     #      errors
     logicalSize       => 2 * $GB,
     # @ple use a small physical size for the underlying storage so that we
     #      can fill VDO quickly
     physicalSize      => 375 * $MB,
     # @ple This value of VDO slab bit count lets the physical size be small
     slabBits          => $SLAB_BITS_TINY,
    );
##

########################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);

  # If a command line option makes this test run multithreaded, we need a VDO
  # large enough to have 2 zones.
  if (defined($self->{physicalThreadCount})
      && ($self->{physicalThreadCount} > 1)) {
    if (parseBytes($self->{logicalSize}) < 4 * $GB) {
      $self->{logicalSize} = 4 * $GB;
    }
    if (parseBytes($self->{physicalSize}) < 0.75 * $GB) {
      $self->{physicalSize} = 0.75 * $GB;
    }
  }

  $self->SUPER::set_up();
}

#############################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  if ($self->failedTest() && defined($device)
      && $device->isa("Permabit::BlockDevice::VDO")) {
    # Upon failure, save the metadata from the VDO device
    eval {
      $device->dumpMetadata();
    };
  }
  $self->SUPER::tear_down();
}

########################################################################
# Get the number of blocks that can be used for data.
#
# @param stats  The stats returned from getVDOStats.
#
# @return the number of blocks usable for device data.
##
sub getUsableDataBlocks {
  my ($self, $stats) = assertNumArgs(2, @_);
  return $stats->{"physical blocks"} - $stats->{"overhead blocks used"};
}

########################################################################
# Sync the device data out of the page cache.  The fsync command may return
# with a failure because writing a block to a full VDO has an error, so
# just repeat the fsync until it completes without error.
##
sub syncDeviceIgnoringErrors {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  # If we get repeated errors, the fsync may never complete (see VDO-4960).
  my $retries = 0;
  while ($retries++ < 10) {
    eval {
      $device->getMachine()->fsync($device->getSymbolicPath());
    };
    if (! $EVAL_ERROR) {
      return;
    }
  }
  die($EVAL_ERROR);
}

########################################################################
# Verify the data on slices of the device (in parallel).
#
# @param slices  The slices of the device
##
sub verifySlices {
  my ($self, $slices) = assertNumArgs(2, @_);
  my @tasks
    = map { Permabit::VDOTask::SliceOperation->new($_, "verify") } @$slices;
  map { $_->start() } @tasks;
  map { $_->result() } @tasks;
}

1;
