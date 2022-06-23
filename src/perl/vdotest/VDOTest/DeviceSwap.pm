##
# Test changing the backing store while VDO is suspended.
#
# $Id$
##
package VDOTest::DeviceSwap;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs assertEqualNumeric);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Data set size
     blockCount    => 1000,
     # @ple A filesystem would be unnecessary complication here
     useFilesystem => 0,
     # @ple The type of VDO device to create. Using an unmanaged vdo since
     # lvm sticks a device it manages between vdo and our backing device.
     vdoDeviceType => "vdo",
    );
##

########################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();

  # Make small devices to limit what the test has to copy.
  my $device = $self->getDevice();
  my $storageSize = 10 * $GB;
  my @parameters = (lvmSize       => $storageSize,
                    storageDevice => $device,
                    volumeGroup   => $self->createVolumeGroup($device,
                                                              "dedupevg"),
                   );
  $self->{linearOne} = $self->createTestDevice("linear",
                                               deviceName  => "firstLV",
                                               @parameters);
  $self->{linearTwo} = $self->createTestDevice("linear",
                                               deviceName => "secondLV",
                                               @parameters);
  $self->{vdo} = $self->createTestDevice($self->{vdoDeviceType},
                                         storageDevice => $self->{linearOne});
}

########################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);

  # Resume any suspended devices.
  my $path = $self->{linearOne}->getDevicePath();
  $self->{linearOne}->runOnHost("sudo dmsetup resume $path");

  # Tear down the VDO device first so the storage stack doesn't get confused.
  $self->destroyTestDevice($self->{vdo});
  $self->SUPER::tear_down();
}

#############################################################################
# Test resuming a volume with a different backing store.
##
sub testDeviceSwitch {
  my ($self) = assertNumArgs(1, @_);
  my $firstLinearPath  = $self->{linearOne}->getDevicePath();
  my $secondLinearPath = $self->{linearTwo}->getDevicePath();
  my $vdoName          = $self->{vdo}->getVDODeviceName();
  my $machine          = $self->{vdo}->getMachine();

  # Write some initial data.
  my $slice = $self->createSlice(device => $self->{vdo},
                                 blockCount => $self->{blockCount});
  $slice->write(tag => "initial", direct => 1, sync => 1);
  $slice->verify();

  # Load the new table and then suspend VDO.
  my $newTable = $self->{vdo}->getTable();
  # XXX this swap isn't valid on lvmvdo since it creates a -vdata device
  # between vdo and the backing device with a different offset.
  $newTable =~ s|/dev/\S* |$secondLinearPath |;
  $machine->runSystemCmd("sudo dmsetup reload $vdoName --table \"$newTable\"");
  $machine->runSystemCmd("sudo dmsetup suspend $vdoName");

  # Copy the contents of the first linear device to the second.
  $self->{linearOne}->ddRead(of    => $secondLinearPath,
                             bs    => 4 * $KB,
                             oflag => "direct",
                             conv  => "fsync");

  # Suspend the first linear to prove we don't use it.
  $machine->runSystemCmd("sudo dmsetup suspend $firstLinearPath");

  # Resume VDO to use the new table. This will hang if VDO sends any I/O to
  # the original backing device so put a timeout on this operation.
  $machine->runSystemCmd("timeout -s KILL 60 sudo dmsetup resume $vdoName");

  # Prove that VDO still works.
  $slice->verify();

  my $slice2 = $self->createSlice(device => $self->{vdo},
                                  blockCount => $self->{blockCount},
                                  offset => $self->{blockCount});
  $slice2->write(tag => "second", direct => 1, sync => 1);
  $slice2->verify();

  # Write duplicate copies of all data
  my $slice3 = $self->createSlice(device => $self->{vdo},
                                  blockCount =>  $self->{blockCount},
                                  offset =>  2 * $self->{blockCount});
  $slice3->write(tag => "initial", direct => 1, sync => 1);
  $slice3->verify();

  my $stats = $self->{vdo}->getVDOStats();
  my $expectedBlocksUsed = 2 * $self->{blockCount};
  assertEqualNumeric($expectedBlocksUsed, $stats->{"data blocks used"},
                     "Data blocks used should be $expectedBlocksUsed");
  assertEqualNumeric($self->{blockCount}, $stats->{"dedupe advice valid"},
                     "Original data should deduplicate.");

  my $slice4 = $self->createSlice(device => $self->{vdo},
                                  blockCount =>  $self->{blockCount},
                                  offset =>  3 * $self->{blockCount});
  $slice4->write(tag => "second", direct => 1, sync => 1);
  $slice4->verify();

  $stats = $self->{vdo}->getVDOStats();
  $expectedBlocksUsed = 2 * $self->{blockCount};
  assertEqualNumeric($expectedBlocksUsed, $stats->{"data blocks used"},
                     "Data blocks used should be $expectedBlocksUsed");
  assertEqualNumeric(2 * $self->{blockCount}, $stats->{"dedupe advice valid"},
                     "Original data should deduplicate.");


  # XXX Ideally we would shut the VDO down and start it again in the
  # new location but vdo manager makes this difficult. When we switch
  # to using an lvmvdo device, it should be much easier to implement.
}

1;
