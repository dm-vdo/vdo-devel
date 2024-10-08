##
# Test the vdoFormat application
#
# $Id$
##
package VDOTest::VDOFormatInKernel;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertEvalErrorMatches
  assertNumArgs
  assertRegexpMatches
  assertTrue
);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %PROPERTIES
  = (
     # @ple set up a linear device
     deviceType     => "linear",
     # @ple format in kernel
     formatInKernel => 1,
     # @ple Use a size that will work for the full range of slab sizes
     physicalSize   => 50 * $GB,
    );

########################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();
  $self->createTestDevice("vdo", setupOnCreation => 0);
}

########################################################################
##
sub _tryIllegal {
  my ($self, $paramName, $value, $error) = assertNumArgs(4, @_);

  my $vdoDevice = $self->getDevice();
  my $machine = $vdoDevice->getMachine();

  my $preformatCursor = $machine->getKernelJournalCursor();
  eval {
    $vdoDevice->formatVDO({ $paramName => $value });
  };
  assertEvalErrorMatches(qr| reload ioctl on .* failed: Invalid argument|);
  assertTrue($machine->searchKernelJournalSince($preformatCursor, $error));
}

########################################################################
# This is temporary till the actual formatting code goes in.
##
sub _tryLegal {
  my ($self, $paramName, $value) = assertNumArgs(3, @_);

  my $vdoDevice = $self->getDevice();
  my $machine = $vdoDevice->getMachine();

  my $preformatCursor = $machine->getKernelJournalCursor();
  eval {
    $vdoDevice->formatVDO({ $paramName => $value });
  };
  assertEvalErrorMatches(qr| reload ioctl on .* failed: Input/output error|);
  assertTrue($machine->searchKernelJournalSince($preformatCursor,
                                                "vdo is not formatted"));
  assertTrue($machine->searchKernelJournalSince($preformatCursor,
                                                "Could not load geometry block"));
}

########################################################################
##
sub testOptions {
  my ($self) = assertNumArgs(1, @_);
  my $maxUInt = ~0;
  my $oneMoreThanMax = "18446744073709551616";
  my $vdoDevice = $self->getDevice();

  $self->_tryLegal("slabBits", 14);
  $self->_tryLegal("slabBits", 17);

  my $slabBitsError = "invalid slab bits";
  $self->_tryIllegal("slabBits",              -2, $slabBitsError);
  $self->_tryIllegal("slabBits",               3, $slabBitsError);
  $self->_tryIllegal("slabBits",              25, $slabBitsError);
  $self->_tryIllegal("slabBits",  ($maxUInt - 1), $slabBitsError);
  $self->_tryIllegal("slabBits",        $maxUInt, $slabBitsError);
  $self->_tryIllegal("slabBits", $oneMoreThanMax, $slabBitsError);

  $self->_tryLegal("logicalSize", 1024);
  $self->_tryLegal("logicalSize", "4096B");
  $self->_tryLegal("logicalSize", "4K");
  $self->_tryLegal("logicalSize", "4M");
  $self->_tryLegal("logicalSize", "1G");
  $self->_tryLegal("logicalSize", "4T");
  $self->_tryLegal("logicalSize", "4P");
  $self->_tryLegal("logicalSize", "4398046511104K");

  # This doesn't work. dmsetup accepts negatives but gets converted to u64 in kernel.
# $self->_tryIllegal("logicalSize",            "-4K", "Usage:");
  $self->_tryIllegal("logicalSize",             "1K", "must be a multiple of");
  $self->_tryIllegal("logicalSize", "4398046511616B", "must be a multiple of");
  $self->_tryIllegal("logicalSize", "4398046511108K", "exceeds the maximum");
  $self->_tryIllegal("logicalSize", ($maxUInt >> 20), "exceeds the maximum");
  $self->_tryIllegal("logicalSize",    "${maxUInt}K", "is zero");
  $self->_tryIllegal("logicalSize",    "${maxUInt}M", "is zero");
  $self->_tryIllegal("logicalSize",    "${maxUInt}G", "is zero");
  $self->_tryIllegal("logicalSize",    "${maxUInt}T", "is zero");
  $self->_tryIllegal("logicalSize",    "${maxUInt}P", "is zero");
  $self->_tryIllegal("logicalSize",    "${maxUInt}E", "is zero");
  $self->_tryIllegal("logicalSize",     "${maxUInt}", "is zero");
  $self->_tryIllegal("logicalSize",  $oneMoreThanMax, "is zero");
  # This doesn't work. parseBytes in Utils.pm knows nothing about Q as a suffix.
# $self->_tryIllegal("logicalSize",             "1Q", "is zero");

  $self->_tryLegal("albireoMem", .25);
  $self->_tryLegal("albireoMem", .5);
  $self->_tryLegal("albireoMem", 1);

  $self->_tryIllegal("albireoMem", 255, "Out of space");

  $self->_tryLegal("albireoSparse", 0);
  $self->_tryLegal("albireoSparse", 1);
}

########################################################################
##
sub testMinimumSize {
  my ($self) = assertNumArgs(1, @_);

  my $vdoDevice = $self->getDevice();
  my $machine = $vdoDevice->getMachine();

  my $preformatCursor = $machine->getKernelJournalCursor();
  eval {
    $vdoDevice->formatVDO({ albireoMem => 4, slabBits => 23 });
  };
  assertEvalErrorMatches(qr| reload ioctl on .* failed: Invalid argument|);

  my $journal = $machine->getKernelJournalSince($preformatCursor);
  assertRegexpMatches(qr/Out of space/, $journal);

  my $msg = ($journal =~ qr/Minimum required size for VDO volume: ([0-9]+) bytes/);
  assertDefined($msg);
  my $physicalSize = $1;

  my $storageDevice = $vdoDevice->getStorageDevice();

  # We round up to physicalExtentSize when extending, so this is the best we
  # can do in terms of checking if less space fails
  my $lessThanNeeded
    = $physicalSize - $storageDevice->{volumeGroup}->{physicalExtentSize} - 1;

  $storageDevice->extend($lessThanNeeded);

  my $extend1Cursor = $machine->getKernelJournalCursor();
  eval {
    $vdoDevice->formatVDO({ albireoMem => 4, slabBits => 23 });
  };
  assertEvalErrorMatches(qr| reload ioctl on .* failed: Invalid argument|);
  assertTrue($machine->searchKernelJournalSince($extend1Cursor, "Out of space"));

  $storageDevice->extend($physicalSize);
  my $extend2Cursor = $machine->getKernelJournalCursor();
  eval {
    $vdoDevice->formatVDO({ albireoMem => 4, slabBits => 23 });
  };
  assertEvalErrorMatches(qr| reload ioctl on .* failed: Input/output error|);
  assertTrue($machine->searchKernelJournalSince($extend2Cursor,
                                                "Could not load geometry block"));
}

1;
