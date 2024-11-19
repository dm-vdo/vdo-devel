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
  my ($self, $paramName, $value, $error, $kernelError) = assertNumArgs(5, @_);

  my $vdoDevice = $self->getDevice();
  my $machine = $vdoDevice->getMachine();

  my $preformatCursor = $machine->getKernelJournalCursor();
  eval {
    $vdoDevice->formatVDO({ $paramName => $value });
  };
  assertEvalErrorMatches(qr/ reload ioctl on .* failed:.*\Q$error\E/);
  assertTrue($machine->searchKernelJournalSince($preformatCursor, $kernelError));
}

########################################################################
##
sub testOptions {
  my ($self) = assertNumArgs(1, @_);
  my $maxUInt = ~0;
  my $oneMoreThanMax = "18446744073709551616";
  my $vdoDevice = $self->getDevice();

  $vdoDevice->formatVDO({ slabBits => 14 });
  $vdoDevice->formatVDO({ slabBits => 17 });

  my $kernelError = "invalid slab bits";
  my $dmsetupError = "Invalid argument";
  $self->_tryIllegal("slabBits",              -2, $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits",               3, $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits",              25, $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits",  ($maxUInt - 1), $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits",        $maxUInt, $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits", $oneMoreThanMax, $dmsetupError, $kernelError);

  $vdoDevice->formatVDO({ logicalSize => 1024 });
  $vdoDevice->formatVDO({ logicalSize => "4096B" });
  $vdoDevice->formatVDO({ logicalSize => "4K" });
  $vdoDevice->formatVDO({ logicalSize => "4M" });
  $vdoDevice->formatVDO({ logicalSize => "1G" });
  $vdoDevice->formatVDO({ logicalSize => "4T" });
  # This doesn't work as it needs more memory than our lab machines have.
#  $vdoDevice->formatVDO({ logicalSize => "4P" });
#  $vdoDevice->formatVDO({ logicalSize => "4398046511104K" });

  # This doesn't work. dmsetup accepts negatives but gets converted to u64 in kernel.
# $self->_tryIllegal("logicalSize",            "-4K", "Usage:");

  $kernelError = "must be a multiple of";
  $self->_tryIllegal("logicalSize",             "1K", $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize", "4398046511616B", $dmsetupError, $kernelError);

  $kernelError = "exceeds the maximum";
  $self->_tryIllegal("logicalSize", "4398046511108K", $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize", ($maxUInt >> 20), $dmsetupError, $kernelError);

  $kernelError = "cannot be zero";
  $self->_tryIllegal("logicalSize",    "${maxUInt}K", $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize",    "${maxUInt}M", $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize",    "${maxUInt}G", $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize",    "${maxUInt}T", $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize",    "${maxUInt}P", $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize",    "${maxUInt}E", $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize",     "${maxUInt}", $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize",  $oneMoreThanMax, $dmsetupError, $kernelError);
  # This doesn't work. parseBytes in Utils.pm knows nothing about Q as a suffix.
# $self->_tryIllegal("logicalSize",             "1Q", "is zero");

  $vdoDevice->formatVDO({ albireoMem => .25 });
  $vdoDevice->formatVDO({ albireoMem => .5 });
  $vdoDevice->formatVDO({ albireoMem => 1 });

  $dmsetupError = "No space left";
  $kernelError = "Not enough space";
  $self->_tryIllegal("albireoMem", 255, $dmsetupError, $kernelError);

  $vdoDevice->formatVDO({ albireoSparse => 0 });
  $vdoDevice->formatVDO({ albireoSparse => 1 });
}

#############################################################################
# Properties to make sure physical threads = slab count.
##
sub propertiesMinimumSize {
  return (
	  physicalThreadCount => 1,
         );
}

########################################################################
##
sub testMinimumSize {
  my ($self) = assertNumArgs(1, @_);

  my $vdoDevice = $self->getDevice();
  my $machine = $vdoDevice->getMachine();

  my $preformatCursor = $machine->getKernelJournalCursor();
  eval {
    $vdoDevice->formatVDO({ albireoMem => 2, slabBits => 23 });
  };
  assertEvalErrorMatches(qr| reload ioctl on .* failed: No space left|);

  my $journal = $machine->getKernelJournalSince($preformatCursor);
  assertRegexpMatches(qr/Not enough space/, $journal);

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
    $vdoDevice->formatVDO({ albireoMem => 2, slabBits => 23 });
  };
  assertEvalErrorMatches(qr| reload ioctl on .* failed: No space left|);
  assertTrue($machine->searchKernelJournalSince($extend1Cursor, "Not enough space"));

  $storageDevice->extend($physicalSize);
  $vdoDevice->formatVDO({ albireoMem => 2, slabBits => 23 });
}

1;
