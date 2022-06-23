##
# Basic VDO test using block read/write
#
# $Id$
##
package VDOTest::Direct01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks to write
     blockCount => 5000,
     # @ple Use a VDO device
     deviceType => "lvmvdo",
     # @ple VDO slab bit count
     slabBits   => $SLAB_BITS_SMALL,
    );
##

########################################################################
# Basic VDO testing.
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  my $expectedStats = {
    "data blocks used"       => 0,
    "dedupe advice valid"    => 0,
    "dedupe advice stale"    => 0,
    "dedupe advice timeouts" => 0,
    "entries indexed"        => 0,
  };
  $self->assertVDOStats($expectedStats);

  # Write some blocks, read them back and verify the data have not changed.
  my $slice1 = $self->createSlice(blockCount => $self->{blockCount});
  $slice1->write(tag => "Direct1", direct => 1,);
  $slice1->verify();
  $expectedStats->{"data blocks used"} += $self->{blockCount};
  $expectedStats->{"entries indexed"}  += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Write the blocks again, expecting complete dedupe.
  my $slice2 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => $self->{blockCount},);
  $slice2->write(tag => "Direct1", direct => 1,);

  $expectedStats->{"dedupe advice valid"} += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Restart the device to verify that data is persistent
  $device->restart();

  # Verify the data have not changed
  $slice1->verify();
  $slice2->verify();
}

1;
