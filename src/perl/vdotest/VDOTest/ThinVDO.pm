##
# Basic VDO test using block read/write
#
# $Id$
##
package VDOTest::ThinVDO;

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
     deviceType => "linear",
     # @ple VDO slab bit count
     slabBits   => $SLAB_BITS_SMALL,
    );
##

########################################################################
# Check LVM version.
##
sub doesLVMSupportThinVDO {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  # Check lvm version. line we want is: 'LVM version:     2.03.26(2) (2024-08-23)'
  $machine->runSystemCmd("sudo lvm version | grep 'LVM version:' | awk '{print \$3}'");
  my $stdout = $machine->getStdout();
  my @version = split(/\(/, $stdout);
  my ($major, $minor, $patch) = split(/\./, $version[0]);
  $log->info("LVM version is " . join(".", ($major, $minor, $patch)));
  if (($major > 2) || ($major == 2 && $minor > 3)
      || ($major == 2 && $minor == 3 && $patch >= 26)) {
    return 1;
  }
  return 0;
}

########################################################################
# Basic VDO testing.
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);

  if (!$self->doesLVMSupportThinVDO()) {
    $log->info("LVM version doesn't support thinpool using vdo");
    return;
  };

  $self->createTestDevice("thinpoolvdo");
  $self->createTestDevice("thin");

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
  $slice1->write(tag => "Direct1", direct => 1);
  $slice1->verify();
  $expectedStats->{"data blocks used"} += $self->{blockCount};
  $expectedStats->{"entries indexed"}  += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Write the blocks again, expecting complete dedupe.
  my $slice2 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => $self->{blockCount},);
  $slice2->write(tag => "Direct1", direct => 1);

  $expectedStats->{"dedupe advice valid"} += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Restart the device to verify that data is persistent
  $device->restart();

  # Verify the data have not changed
  $slice1->verify();
  $slice2->verify();
}

########################################################################
# VDO testing for Multiple Thin Volumes.
##
sub testMultiple {
  my ($self) = assertNumArgs(1, @_);

  if (!$self->doesLVMSupportThinVDO()) {
    $log->info("LVM version doesn't support thinpool using vdo");
    return;
  };

  $self->createTestDevice("thinpoolvdo");

  my $storageDevice = $self->getDevice();
  my $machine = $storageDevice->getMachine();

  # split the raw device in half and deduct the size of the albireo index
  my $storageSize = $storageDevice->getSize() / 2;
  # Create the appropriate storage devices for each VDO.
  my @parameters = (storageDevice => $storageDevice,
                    volumeGroup   => $storageDevice->{volumeGroup},
                   );
  $self->{firstThinLV}
    = $self->createTestDevice("thin",
                              deviceName  => "firstThin",
                              lvmSize     => $storageSize,
                              @parameters);
  $self->{secondThinLV}
    = $self->createTestDevice("thin",
                              deviceName => "secondThin",
                              @parameters);
  my $expectedStats = {
    "data blocks used"       => 0,
    "dedupe advice valid"    => 0,
    "dedupe advice stale"    => 0,
    "dedupe advice timeouts" => 0,
    "entries indexed"        => 0,
  };
  $self->assertVDOStats($expectedStats);

  # Write some blocks, read them back and verify the data have not changed.
  my $slice1 = $self->createSlice(device => $self->{firstThinLV},
                                  blockCount => $self->{blockCount});
  $slice1->write(tag => "Direct1", direct => 1);
  $slice1->verify();
  $expectedStats->{"data blocks used"} += $self->{blockCount};
  $expectedStats->{"entries indexed"}  += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Write the blocks again to the other thin lv, expecting complete dedupe.
  my $slice2 = $self->createSlice(device => $self->{secondThinLV},
                                  blockCount => $self->{blockCount});
  $slice2->write(tag => "Direct1", direct => 1);

  $expectedStats->{"dedupe advice valid"} += $self->{blockCount};
  $self->assertVDOStats($expectedStats);

  # Restart the device to verify that data is persistent
  $self->{firstThinLV}->restart();
  $self->{secondThinLV}->restart();

  # Verify the data have not changed
  $slice1->verify();
  $slice2->verify();
}

1;
