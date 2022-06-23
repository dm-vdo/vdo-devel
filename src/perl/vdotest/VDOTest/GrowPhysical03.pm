##
# Test that VDO and a filesystem can both be grown.
#
# $Id$
##
package VDOTest::GrowPhysical03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::GenDataFiles qw(genDataFiles);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDO device
     deviceType    => "lvmvdo",
     # @ple Initial logical size
     logicalSize   => 20 * $GB,
     # @ple Initial physical size
     physicalSize  => 5 * $GB,
     # @ple The number of bits in the VDO slab
     slabBits      => $SLAB_BITS_TINY,
     # @ple Whether to use a filesystem
     useFilesystem => 1,
    );
##

#############################################################################
# Grow the device, then write enough non-unique data to ensure that
# we write something to the new space.
##
sub testUseNew {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  # device 5G physical/20G logical; make it 10G physical/20G logical
  $device->growPhysical(10 * $GB);

  # Now write 6G and watch it not run out of space
  my $dataSet = genDataFiles(
                             fs       => $self->getFileSystem(),
                             numBytes => 6 * $GB,
                             numFiles => 6,
                            );
  $dataSet->verify();
}

1
