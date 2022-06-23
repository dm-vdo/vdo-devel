##
# Test the Permabit::MegaRaid utilities
#
# These tests rely on a known mock configuration in ./MockMegaRaid
#
# $Id$
##
package testcases::MegaRaid_t1;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertEqualNumeric
  assertFalse
  assertNENumeric
  assertNe
  assertNumArgs
  assertNumDefinedArgs
  assertTrue
);
use Permabit::MegaRaid::MockMegaRaid::Adapter;
use Permabit::MegaRaid::MockMegaRaid::FakeUserMachine;
use Permabit::MegaRaid::Utils qw(parseCliResult);

use base qw(Permabit::Testcase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The RSVP class clients should be reserved from
     clientClass => undef,
     # @ple The names of the machines to be used
     clientNames => undef,
     # @ple The number of clients that will be used
     numClients  => 1,
    );
##

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumDefinedArgs(1, @_);
  $self->SUPER::set_up();
  $self->reserveHostGroup("client");
  $self->{_machine} = Permabit::MegaRaid::MockMegaRaid::FakeUserMachine->new(
    hostname    => $self->{clientNames}[0],
    nfsShareDir => $self->{nfsShareDir},
    scratchDir  => $self->{workDir},
    workDir     => $self->{workDir}
  );
}

#############################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumDefinedArgs(1, @_);
  $self->{_machine}->close();
  delete $self->{_machine};
  $self->SUPER::tear_down();
}

#############################################################################
# @inherit
##
sub getTestHosts {
  my ($self) = assertNumArgs(1, @_);
  my @hosts = $self->SUPER::getTestHosts();
  if (ref($self->{clientNames}) eq "ARRAY") {
    @hosts = (@hosts, @{$self->{clientNames}});
  }
  return @hosts;
}

######################################################################
# Basic test asserting that our MegaRaid object was able to parse some
#  known -AdpAllInfo command output upon initialization
##
sub testGetAdapterInfo {
  my ($self) = assertNumDefinedArgs(1, @_);
  my $mockAdapter = $self->_getAdapter();

  # Assert values we know based on our MockMegaRaid objects
  assertEqualNumeric(4,      scalar(@{$mockAdapter->getPhysDisks()}));
  assertEqualNumeric(2,      scalar(@{$mockAdapter->getVirtualDevices()}));
  assertEqualNumeric(0,      $mockAdapter->{failedDiskCt});
  assertEq("2.120.184-1415", $mockAdapter->{fwVers});
}

######################################################################
##
sub testGetVirtualDevicesInfo {
  my ($self) = assertNumDefinedArgs(1, @_);
  my $mockAdapter = $self->_getAdapter();
  my $vds         = $mockAdapter->getVirtualDevices();

  # Assert with found both devices
  assertEqualNumeric(2, scalar(@{$vds}));

  # Assert values we know based on our MockMegaRaid objects
  foreach my $vd (@{$vds}) {
    my $config = $vd->getConfig();
    assertEqualNumeric(0,      $config->{raidType});
    assertEqualNumeric(2,      scalar($vd->getDisks()));
    assertEq("Direct",         $config->{deviceCachePol});
    assertEqualNumeric("64",   $config->{stripeSize});
    assertEq("WriteThrough",   $config->{writeCachePol});
    assertEq("ReadAheadNone",  $config->{readCachePol});
    assertEq("Disk's Default", $config->{diskCachePol});
  }
}

######################################################################
##
sub testAddVirtualDevice {
  my ($self) = assertNumDefinedArgs(1, @_);
  my $mockAdapter = $self->_getAdapter();

  # Get the 1st VirtualDevice for later comparison
  my ($vd1, $vd2) = @{$mockAdapter->getVirtualDevices()};
  my ($vdc1, $vdc2) = map {$_->getConfig()} ($vd1, $vd2);
  assertFalse($vdc1->isAllDisksFree());
  assertFalse($vdc2->isAllDisksFree());

  # Make sure the configs don't match
  assertNe($vdc1, $vdc2);

  # Remove the 2nd VirtualDevice and make sure it got destroyed
  $mockAdapter->destroyVirtualDevice($vd2);
  assertFalse($vdc1->isAllDisksFree());
  assertTrue($vdc2->isAllDisksFree());
  assertEqualNumeric(0, scalar($vd2->getDisks()));

  # Add it back
  $vd2 = $mockAdapter->createVirtualDevice($vdc2);
  assertFalse($vdc1->isAllDisksFree());
  assertFalse($vdc2->isAllDisksFree());

  # Check that newly fetched 2nd VirtualDevice is the same object
  #  and config as the one we removed and re-added.
  assertEqualNumeric($vd2, $mockAdapter->getVirtualDevices()->[1]);
  assertEq($vdc2, $vd2->getConfig());

  # Check that VirtualDevice 1 is fetchable, the same object, and
  #  with the same configuration.
  assertEqualNumeric($vd1, $mockAdapter->getVirtualDevices()->[0]);
  assertEq($vdc1, $vd1->getConfig());
}

######################################################################
##
sub testDiskFields {
  my ($self) = assertNumDefinedArgs(1, @_);
  my $mockAdapter = $self->_getAdapter();

  # test the 1st VirtualDevice
  my $vd1   = $mockAdapter->getVirtualDevices()->[0];
  my @disks = $vd1->getDisks();

  # should have found 2 disk
  assertEqualNumeric(2, scalar(@disks));

  # Assert values we know based on our MockMegaRaid objects
  assertEqualNumeric(0,          $disks[0]->{slotNum});
  assertEqualNumeric(7,          $disks[0]->{deviceId});
  assertEqualNumeric(64,         $disks[0]->{enclosureId});
  assertEq("/dev/disk/by-path/pci-0000:02:00.0-scsi-0:2:0:0",
           $disks[0]->getDevicePath());
  assertEqualNumeric(1,          $disks[1]->{slotNum});
  assertEqualNumeric(5,          $disks[1]->{deviceId});
  assertEqualNumeric(64,         $disks[1]->{enclosureId});
  assertEq("/dev/disk/by-path/pci-0000:02:00.0-scsi-0:2:0:0",
           $disks[1]->getDevicePath());

  # test the 2nd VirtualDevice
  my $vd2 = $mockAdapter->getVirtualDevices()->[1];
  @disks  = $vd2->getDisks();

  # should have found 1 disk
  assertEqualNumeric(2, scalar(@disks));

  # Assert values we know based on our MockMegaRaid objects
  assertEqualNumeric(2,          $disks[0]->{slotNum});
  assertEqualNumeric(9,          $disks[0]->{deviceId});
  assertEqualNumeric(64,         $disks[0]->{enclosureId});
  assertEq("/dev/disk/by-path/pci-0000:02:00.0-scsi-0:2:1:0",
           $disks[0]->getDevicePath());
  assertEqualNumeric(3,          $disks[1]->{slotNum});
  assertEqualNumeric(4,          $disks[1]->{deviceId});
  assertEqualNumeric(64,         $disks[1]->{enclosureId});
  assertEq("/dev/disk/by-path/pci-0000:02:00.0-scsi-0:2:1:0",
           $disks[1]->getDevicePath());
}

######################################################################
##
sub testOverloads {
  my ($self) = assertNumDefinedArgs(1, @_);
  my $mockAdapter = $self->_getAdapter();
  my ($vd1, $vd2) = @{$mockAdapter->getVirtualDevices()};
  my ($vdc1, $vdc2) = map {$_->getConfig()} ($vd1, $vd2);
  my ($pd1, $pd2) = @{$mockAdapter->getPhysDisks()};

  $log->debug("$vd1");
  $log->debug("$vdc1");
  $log->debug("$pd1");
  assertNe($vd1, $vd2);
  assertNe($vdc1, $vdc2);
  assertEq($vd1, $vd1);
  assertEq($vdc1, $vdc1);
  assertEqualNumeric($vd1, $vd1);
  assertEqualNumeric($vdc1, $vdc1);
  assertEqualNumeric($pd1, $pd1);
  assertNENumeric($vd1, $vd2);
  assertNENumeric($vdc1, $vdc2);
  assertNENumeric($pd1, $pd2);

  # Not implemented yet
  #assertNe($pd1, $pd2);
  #assertEq($pd1, $pd1);
}

######################################################################
##
sub testEquality {
  my ($self) = assertNumDefinedArgs(1, @_);
  my $mockAdapter = $self->_getAdapter();
  my ($vd1, $vd2) = @{$mockAdapter->getVirtualDevices()};
  my ($vdc1, $vdc2) = map {$_->getConfig()} ($vd1, $vd2);
  my ($pd1, $pd2) = @{$mockAdapter->getPhysDisks()};

  # Sanity checks
  assertNe($vd1, $vd2);
  assertNe($vdc1, $vdc2);
  assertEq($vd1, $vd1);
  assertEq($vdc1, $vdc1);
  assertEqualNumeric($vd1, $vd1);
  assertEqualNumeric($vdc1, $vdc1);
  assertEqualNumeric($pd1, $pd1);
  assertNENumeric($vd1, $vd2);
  assertNENumeric($vdc1, $vdc2);
  assertNENumeric($pd1, $pd2);

  # Assert that copies are identical but not the same instance.
  my ($vd1prime, $vdc1prime) = map {$_->clone()} ($vd1, $vdc1);
  assertEq($vd1, $vd1prime);
  assertEq($vdc1, $vdc1prime);
  assertNENumeric($vd1, $vd1prime);
  assertNENumeric($vdc1, $vdc1prime);

  # Assert that getConfig() returns a copy
  assertEq($vd1->getConfig(), $vd1prime->getConfig());
  assertNENumeric($vd1->getConfig(), $vd1prime->getConfig());
  assertEq($vd1->{_config}, $vd1prime->getConfig());
  assertNENumeric($vd1->{_config}, $vd1prime->getConfig());

  # Assert that the cloned objects actually have the same disk instances
  my @disks = $vd1->getDisks();
  my @disksPrime = $vd1prime->getDisks();
  for (my $i = 0; $i <= $#disks; ++$i) {
    assertEqualNumeric($disks[$i], $disksPrime[$i]);
  }
}

######################################################################
##
sub testRemoveVirtualDevice {
  my ($self) = assertNumDefinedArgs(1, @_);
  my $mockAdapter = $self->_getAdapter();

  my $virtualDevice = $mockAdapter->getVirtualDevices()->[0];
  my $vdc = $virtualDevice->getConfig();
  assertFalse($vdc->isAllDisksFree());

  # Remove the virtual device from the adapter
  $mockAdapter->destroyVirtualDevice($virtualDevice);

  # Assert that the disks were released
  assertTrue($vdc->isAllDisksFree());

  # Assert that our VirtualDeviceConfig still has PhysicalDisk objects
  #  and that those objects are are not in use.
  my @disks = $vdc->getDisks();
  assertEqualNumeric(2, scalar(@disks));
  foreach my $disk (@disks) {
    assertFalse($disk->isInUse());
  }

  # Assert that our Adapter doesn't return undefined devices and that
  #  our only remaining device is still configured
  assertEqualNumeric(1, scalar(@{$mockAdapter->getVirtualDevices()}));
  $vdc = $mockAdapter->getVirtualDevices()->[0]->getConfig();
  assertFalse($vdc->isAllDisksFree());
}

######################################################################
##
sub testParseCliResultValid {
  my ($self) = assertNumDefinedArgs(1, @_);

  my $wanted = { a => "x", b => "y" };

  # test a normal passing scenario (leave the extra spaces there)
  my $string = <<STR;
a  :  abc
foo
bar
 b : bcd
 foo : bar
a : abc
 b : xyz
STR
  my $res = parseCliResult($string, $wanted);

  # should have two groups
  assertEqualNumeric(2, scalar(@{$res}));

  assertDefined(  $res->[0]->{x});
  assertEq("abc", $res->[0]->{x});
  assertDefined(  $res->[0]->{y});
  assertEq("bcd", $res->[0]->{y});

  assertDefined(  $res->[1]->{x});
  assertEq("abc", $res->[1]->{x});
  assertDefined(  $res->[1]->{y});
  assertEq("xyz", $res->[1]->{y});
}

######################################################################
##
sub testParseCliResultInvalid {
  my ($self) = assertNumDefinedArgs(1, @_);

  my $wanted = { a => "x", b => "y" };

  # test a failing scenario.  the 2nd instance of key "a " comes before the
  #  first instance of key "b" ... so "a" is a duplicate key.
  my $string = <<STR;
 a  :  abc
foo
 bar
a : abc
 b : xyz
STR
  eval {
    my $res = parseCliResult($string, $wanted);
  };
  assertDefined($EVAL_ERROR);
  assertTrue($EVAL_ERROR =~ /Duplicate key: a/ || 0);
}

######################################################################
# Get a MockMegaRaid::Adapter
#
# @return a MockMegaRaid::Adapter
##
sub _getAdapter {
  my ($self) = assertNumDefinedArgs(1, @_);
  return Permabit::MegaRaid::MockMegaRaid::Adapter->new(
    machine => $self->{_machine},
  );
}
