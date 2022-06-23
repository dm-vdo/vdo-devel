##
# Basic VDO test using block read/write
#
# $Id$
##
package VDOTest::Direct06;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use POSIX qw(ceil);

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
    "bios in write"          => 0,
    "bios out write"         => 0,
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
  $expectedStats->{"bios in write"}    += $self->{blockCount};
  $expectedStats->{"bios out write"}   += $self->{blockCount};
  $expectedStats->{"entries indexed"}  += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Write the blocks again, expecting complete dedupe.
  my $slice2 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => $self->{blockCount},);
  $slice2->write(tag => "Direct1", direct => 1,);
  $expectedStats->{"dedupe advice valid"} += $self->{blockCount};
  $expectedStats->{"bios in write"}       += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Restart the device to verify that data are persistent.
  $device->restart();
  $expectedStats = {
    "bios in write"       => 0,
    "bios out write"      => 0,
    "data blocks used"    => $self->{blockCount},
    "dedupe advice valid" => 0,
    "dedupe advice stale" => 0,
    "entries indexed"     => $self->{blockCount},

    # Make sure we don't mistakenly do partial I/O or misaccount (VDO-4248)
    "bios in partial read"              => 0,
    "bios in partial write"             => 0,
    "bios in partial discard"           => 0,
    "bios in partial flush"             => 0,
    "bios in partial fua"               => 0,
    "bios acknowledged partial read"    => 0,
    "bios acknowledged partial write"   => 0,
    "bios acknowledged partial discard" => 0,
    "bios acknowledged partial flush"   => 0,
    "bios acknowledged partial fua"     => 0,
  };

  # Verify the data have not changed.
  $slice1->verify();
  $slice2->verify();
  $self->assertVDOStats($expectedStats);

  # Trim the first dataset. It should not reduce data blocks used.
  my $machine = $device->getMachine();
  my $trimsPerDataset
    = ceil($self->{blockCount} / $self->_getMaxDiscardBlocks());
  $slice1->trim();
  $machine->dropCaches();
  $slice1->verify();
  $slice2->verify();
  $expectedStats->{"bios in discard"} += $trimsPerDataset;
  $expectedStats->{"bios in write"}   = 0;
  $self->assertVDOStats($expectedStats);

  # Trim the second dataset. It should reduce data blocks used.
  $slice2->trim();
  $machine->dropCaches();
  $slice1->verify();
  $slice2->verify();
  $expectedStats->{"data blocks used"} -= $self->{blockCount};
  $expectedStats->{"bios in discard"}  += $trimsPerDataset;
  $expectedStats->{"bios in write"}    = 0;
  $self->assertVDOStats($expectedStats);

  # Write new data to the first slice.
  $slice1->write(tag => "Direct2", direct => 1,);
  $slice1->verify();

  $expectedStats->{"bios in write"}    += $self->{blockCount};
  $expectedStats->{"bios out write"}   += $self->{blockCount};
  $expectedStats->{"data blocks used"} += $self->{blockCount};
  $expectedStats->{"entries indexed"}  += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Rewrite that dataset atop its old location. The idea is to test writing
  # block X onto address A which already had block X written to it.
  $slice1->write(tag => "Direct2", direct => 1,);
  $slice1->verify();

  $expectedStats->{"dedupe advice valid"} += $self->{blockCount};
  $expectedStats->{"bios in write"}       += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Henceforth dedupe advice and bios out write are nondeterministic.
  delete($expectedStats->{"bios out write"});
  delete($expectedStats->{"dedupe advice stale"});
  delete($expectedStats->{"dedupe advice valid"});
  # Bios in write can be off by one (due to flushes) depending on platform.
  delete($expectedStats->{"bios in write"});

  # Write new data to slice 2.
  $slice2->write(tag => "Direct5", fsync => 1,);
  $slice2->verify();
  $expectedStats->{"data blocks used"} += $self->{blockCount};
  $expectedStats->{"entries indexed"}  += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Write the same dataset shifted over one block, to test deduping against
  # data being overwritten.
  my $slice3 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => ($self->{blockCount} + 1),);
  $slice3->write(tag => "Direct5", fsync => 1,);
  $slice3->verify();
  $self->assertVDOStats($expectedStats);

  # Rewrite the dataset in the original location.
  $slice2->write(tag => "Direct5", fsync => 1,);
  $slice2->verify();
  $self->assertVDOStats($expectedStats);

  # Trim slice 1 and verify that slice 2 is unharmed.
  $slice1->trim();
  $expectedStats->{"data blocks used"} -= $self->{blockCount};
  $expectedStats->{"bios in discard"}  += $trimsPerDataset;
  $self->assertVDOStats($expectedStats);
  $machine->dropCaches();
  $slice1->verify();
  $slice2->verify();

  # Trim slice 2 and verify that a single block is still mapped.
  $slice2->trim();
  $expectedStats->{"data blocks used"} -= ($self->{blockCount} - 1);
  $expectedStats->{"bios in discard"}  += $trimsPerDataset;
  $machine->dropCaches();
  $slice2->verify();
}

########################################################################
# Get the maximum discard blocks
##
sub _getMaxDiscardBlocks {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  return $device->getMaxDiscardSectors() * $SECTOR_SIZE / $self->{blockSize};
}

1;
