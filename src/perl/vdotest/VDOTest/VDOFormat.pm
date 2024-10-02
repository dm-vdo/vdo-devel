##
# Test the vdoFormat application
#
# $Id$
##
package VDOTest::VDOFormat;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertEvalErrorMatches
  assertNumArgs
);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %PROPERTIES
  = (
     # @ple set up a linear device
     deviceType => "linear",
     # @ple Use a size that will work for the full range of slab sizes
     physicalSize => 50 * $GB,
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
  eval {
    $vdoDevice->formatVDO({ $paramName => $value });
  };
  assertEvalErrorMatches(qr/vdoformat:.*\Q$error\E/);
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

  my $slabBitsError = "invalid slab bits";
  $self->_tryIllegal("slabBits",              -2, $slabBitsError);
  $self->_tryIllegal("slabBits",               3, $slabBitsError);
  $self->_tryIllegal("slabBits",              25, $slabBitsError);
  $self->_tryIllegal("slabBits",  ($maxUInt - 1), $slabBitsError);
  $self->_tryIllegal("slabBits",        $maxUInt, $slabBitsError);
  $self->_tryIllegal("slabBits", $oneMoreThanMax, $slabBitsError);

  $vdoDevice->formatVDO({ logicalSize => 1024             });
  $vdoDevice->formatVDO({ logicalSize => "4096B"          });
  $vdoDevice->formatVDO({ logicalSize => "4K"             });
  $vdoDevice->formatVDO({ logicalSize => "4M"             });
  $vdoDevice->formatVDO({ logicalSize => "1G"             });
  $vdoDevice->formatVDO({ logicalSize => "4T"             });
  $vdoDevice->formatVDO({ logicalSize => "4P"             });
  $vdoDevice->formatVDO({ logicalSize => "4398046511104K" });

  $self->_tryIllegal("logicalSize",            "-4K", "Usage:");
  $self->_tryIllegal("logicalSize",             "1K", "must be a multiple of");
  $self->_tryIllegal("logicalSize", "4398046511108B", "must be a multiple of");
  $self->_tryIllegal("logicalSize", "4398046511108K", "exceeds the maximum");
  $self->_tryIllegal("logicalSize", ($maxUInt >> 20), "exceeds the maximum");
  $self->_tryIllegal("logicalSize",    "${maxUInt}K", "Usage:");
  $self->_tryIllegal("logicalSize",    "${maxUInt}M", "Usage:");
  $self->_tryIllegal("logicalSize",    "${maxUInt}G", "Usage:");
  $self->_tryIllegal("logicalSize",    "${maxUInt}T", "Usage:");
  $self->_tryIllegal("logicalSize",    "${maxUInt}P", "Usage:");
  $self->_tryIllegal("logicalSize",    "${maxUInt}E", "Usage:");
  $self->_tryIllegal("logicalSize",     "${maxUInt}", "Usage:");
  $self->_tryIllegal("logicalSize",  $oneMoreThanMax, "Usage:");
  $self->_tryIllegal("logicalSize",             "1Q", "Usage:");

  $vdoDevice->formatVDO({ albireoMem => .25 });
  $vdoDevice->formatVDO({ albireoMem =>  .5 });
  $vdoDevice->formatVDO({ albireoMem =>   1 });

  $self->_tryIllegal("albireoMem", 255, "Out of space");

  $vdoDevice->formatVDO({ albireoSparse => 0 });
  $vdoDevice->formatVDO({ albireoSparse => 1 });
}

########################################################################
##
sub testMinimumSize {
  my ($self) = assertNumArgs(1, @_);

  my $vdoDevice = $self->getDevice();
  my $machine = $vdoDevice->getMachine();
  eval {
    $vdoDevice->formatVDO({ albireoMem => 4, slabBits => 23 });
  };
  assertEvalErrorMatches(qr/Out of space/);

  my $msg = ($EVAL_ERROR =~ qr/Minimum required size for VDO volume: ([0-9]+) bytes/);
  assertDefined($msg);
  my $physicalSize = $1;

  my $storageDevice = $vdoDevice->getStorageDevice();

  # We round up to physicalExtentSize when extending, so this is the best we
  # can do in terms of checking if less space fails
  my $lessThanNeeded = $physicalSize
    - $storageDevice->{volumeGroup}->{physicalExtentSize} - 1;

  $storageDevice->extend($lessThanNeeded);
  eval {
    $vdoDevice->formatVDO({ albireoMem => 4, slabBits => 23 });
  };
  assertEvalErrorMatches(qr/Out of space/);

  $storageDevice->extend($physicalSize);
  $vdoDevice->formatVDO({ albireoMem => 4, slabBits => 23 });
}

1;
