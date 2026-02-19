##
# Test the vdoFormat in the kernel code
#
# $Id$
##
package VDOTest::VDOFormatInKernel;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Math::BigInt;

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
# Most of these tests are the same as from the VDOFormat test, but we are
# testing the format in the kernel code instead of the vdoformat application,
# so some messages are different.
##
sub testOptions {
  my ($self) = assertNumArgs(1, @_);
  my $maxUInt = ~0;
  my $oneMoreThanMax = "18446744073709551616";
  my $vdoDevice = $self->getDevice();

  # Validated through the VDO kernel code
  $vdoDevice->formatVDO({ slabBits => 14 });
  $vdoDevice->formatVDO({ slabBits => 17 });

  my $kernelError = "invalid slab size";
  my $dmsetupError = "Invalid argument";
  $self->_tryIllegal("slabBits",              -2, $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits",               3, $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits",              25, $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits",  ($maxUInt - 1), $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits",        $maxUInt, $dmsetupError, $kernelError);
  $self->_tryIllegal("slabBits", $oneMoreThanMax, $dmsetupError, $kernelError);

  # Validated through the VDO kernel code
  $vdoDevice->formatVDO({ logicalSize => 4096 });
  $vdoDevice->formatVDO({ logicalSize => 4 * $MB });
  $vdoDevice->formatVDO({ logicalSize => 1 * $GB });
  $vdoDevice->formatVDO({ logicalSize => 4 * $TB });

  # Validated through the dmsetup code
  $kernelError = "zero-length target";
  $self->_tryIllegal("logicalSize", 0, $dmsetupError, $kernelError);

  # Validated through the VDO kernel code
  $kernelError = "must be a multiple of";
  $self->_tryIllegal("logicalSize",            1024, $dmsetupError, $kernelError);
  $self->_tryIllegal("logicalSize", ((4 * $TB) + 512), $dmsetupError, $kernelError);

  # Validated through the VDO kernel code
  $kernelError = "exceeds the maximum";
  my $bigNum = Math::BigInt->new((4 * $PB) + 512);
  $self->_tryIllegal("logicalSize", $bigNum, $dmsetupError, $kernelError);

  # Validated through the dmsetup code
  $kernelError = "too large device";
  $bigNum = Math::BigInt->new(${maxUInt} * $KB);
  $self->_tryIllegal("logicalSize",  $bigNum, $dmsetupError, $kernelError);
  $bigNum = Math::BigInt->new(${maxUInt} * $MB);
  $self->_tryIllegal("logicalSize",  $bigNum, $dmsetupError, $kernelError);
  $bigNum = Math::BigInt->new(${maxUInt} * $GB);
  $self->_tryIllegal("logicalSize",  $bigNum, $dmsetupError, $kernelError);
  $bigNum = Math::BigInt->new(${maxUInt} * $PB);
  $self->_tryIllegal("logicalSize",  $bigNum, $dmsetupError, $kernelError);
  $bigNum = Math::BigInt->new(${maxUInt} * $TB);
  $self->_tryIllegal("logicalSize",  $bigNum, $dmsetupError, $kernelError);

  $vdoDevice->formatVDO({ albireoMem => .25 });
  $vdoDevice->formatVDO({ albireoMem => .5 });
  $vdoDevice->formatVDO({ albireoMem => 1 });

  $dmsetupError = "No space left";
  $kernelError = "Could not allocate";
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

  # This will fail because 50GB is too too small for the given
  # parameters. So we fail then figure out how much space we
  # actually need and do some sizing to test things with that value
  my $preformatCursor = $machine->getKernelJournalCursor();
  eval {
    $vdoDevice->formatVDO({ albireoMem => 2, slabBits => 23 });
  };
  assertEvalErrorMatches(qr| reload ioctl on .* failed: No space left|);

  my $journal = $machine->getKernelJournalSince($preformatCursor);
  assertRegexpMatches(qr/Could not allocate/, $journal);

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
  assertTrue($machine->searchKernelJournalSince($extend1Cursor, "Could not allocate"));

  $storageDevice->extend($physicalSize);
  $vdoDevice->formatVDO({ albireoMem => 2, slabBits => 23 });
}

########################################################################
##
sub testDirtyStorage {
  my ($self) = assertNumArgs(1, @_);

  my $vdoDevice = $self->getDevice();
  my $storageDevice = $vdoDevice->getStorageDevice();
  $storageDevice->ddWrite(
                          if    => "/dev/urandom",
                          count => $storageDevice->getSize() / $self->{blockSize},
                          bs    => $self->{blockSize},
                         );

  $vdoDevice->formatVDO({ albireoMem => .25 });
}

1;
