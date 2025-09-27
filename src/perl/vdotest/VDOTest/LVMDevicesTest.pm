##
# VDO test that verifies the LVM devices file cleanup works correctly
#
# $Id$
##
package VDOTest::LVMDevicesTest;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEq
  assertNe
  assertNumArgs
  assertRegexpMatches
);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple Don't create any devices automatically
  deviceType => "none",
);
##

#############################################################################
# Test that creating and destroying a raw device properly maintains the
# LVM devices file state and that the system device still exists after destruction
##
sub testRawDevice {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getUserMachine();

  if (!$machine->isLvmdevicesAvailable()) {
    $log->info("LVM devices file is not available ... skipping test");
    return;
  }

  my $rawDevice = $self->createTestDevice("raw");
  my $devicePath = $rawDevice->getDevicePath();

  my $deviceInFile = $machine->isInLVMDevicesFile($devicePath);
  assertEq(1, $deviceInFile, "Device should have been added to LVM devices file");

  $self->destroyTestDevice($rawDevice);

  $deviceInFile = $machine->isInLVMDevicesFile($devicePath);
  assertEq(0, $deviceInFile, "Device should have been removed from LVM devices file");

  my $result = $machine->sendCommand("sudo test -e " . $devicePath);
  assertEq(0, $result, "Device should still exist after destruction");
}

#############################################################################
# Test two scenarios:
# 1. Removing a raw device from the LVM devices file if the system device still exists
# but there are other devices on top of it. This should do nothing.
# 2. Removing a non raw device from the LVM devices file if the system device still
# exists. This should do nothing.
##
sub testBadStack {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getUserMachine();

  if (!$machine->isLvmdevicesAvailable()) {
    $log->info("LVM devices file is not available ... skipping test");
    return;
  }

  my $rawDevice = $self->createTestDevice("raw");
  my $rawDevicePath = $rawDevice->getDevicePath();

  my $deviceInFile = $machine->isInLVMDevicesFile($rawDevicePath);
  assertEq(1, $deviceInFile, "Raw device should have been added to LVM devices file");

  my $vdoDevice = $self->createTestDevice("lvmvdo");
  my $vdoDevicePath = $vdoDevice->getDevicePath();

  $deviceInFile = $machine->isInLVMDevicesFile($vdoDevicePath);
  assertEq(1, $deviceInFile, "VDO device should have been added to LVM devices file");

  # Scenario 1
  $rawDevice->removeFromDevicesFile();

  $deviceInFile = $machine->isInLVMDevicesFile($rawDevicePath);
  assertEq(1, $deviceInFile, "Raw device should still be in LVM devices file after removal attempt");

  # Scenario 2
  $vdoDevice->removeFromDevicesFile();

  $deviceInFile = $machine->isInLVMDevicesFile($vdoDevicePath);
  assertEq(1, $deviceInFile, "VDO device should still be in LVM devices file after removal attempt");

  my $vgName = $vdoDevice->{volumeGroup}->getName();
  my $lvName = $vdoDevice->getDeviceName();
  my $fullLVName = "$vgName/$lvName";

  $self->destroyTestDevice($vdoDevice);

  $deviceInFile = $machine->isInLVMDevicesFile($vdoDevicePath);
  assertEq(0, $deviceInFile, "VDO device should have been removed from LVM devices file");

  # lvs exits with 5 if the VG or LV is not found
  my $result = $machine->sendCommand("sudo lvs '$fullLVName'");
  assertNe(0, $result, "VDO Device should not be found in lvs after destruction");
  my $stderr = $machine->getStderr();
  my $regexp = qr/[Volume group|Logical volume] ".*" not found/;
  assertRegexpMatches($regexp, $stderr);

  $result = $machine->sendCommand("sudo test -e " . $vdoDevicePath);
  assertEq(1, $result, "VDO Device should not exist after destruction");

  $self->destroyTestDevice($rawDevice);

  $deviceInFile = $machine->isInLVMDevicesFile($rawDevicePath);
  assertEq(0, $deviceInFile, "Raw device should have been removed from LVM devices file");

  $result = $machine->sendCommand("sudo test -e " . $rawDevicePath);
  assertEq(0, $result, "Raw device should still exist after destruction");
}

1;
