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
  assertEqualNumeric
  assertFalse
  assertNe
  assertNumArgs
  assertRegexpMatches
  assertTrue
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
# Verify that all devices listed in the LVM devices file currently exist
# on the machine after we cleanup.
##
sub assertLVMDevicesCleanupValid {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getUserMachine();

  $machine->cleanupLVMDevicesFile();

  my @devices = $machine->getValidDevicesInLVMDevicesFile();
  foreach my $device (@devices) {
    $machine->runSystemCmd("sudo test -e '$device->{resolvedName}'");
  }
}

#############################################################################
# Our cleanup code is done in the VDOTest base class after all devices are
# destroyed. It has no notion of different devices behaving differently from
# other devices. This is fine. As long as the device exists in the system,
# having an entry in the LVM devices file is not a problem. Test some stacks
# to verify this.
##
sub testDeviceStack {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getUserMachine();

  if (!$machine->isLvmdevicesAvailable()) {
    $log->info("LVM devices file is not available ... skipping test");
    return;
  }

  my $rawDevice = $self->createTestDevice("raw");
  my $rawDevicePath = $rawDevice->getDevicePath();

  assertTrue($machine->isInLVMDevicesFile($rawDevicePath),
             "Raw device should have been added to LVM devices file");

  my $vdoDevice = $self->createTestDevice("lvmvdo");
  my $vdoDevicePath = $vdoDevice->getDevicePath();

  assertTrue($machine->isInLVMDevicesFile($vdoDevicePath),
             "VDO device should have been added to LVM devices file");

  # Scenario 1 - Cleanup while multiple devices exist
  $self->assertLVMDevicesCleanupValid();

  assertTrue($machine->isInLVMDevicesFile($rawDevicePath),
             "Raw device should still be in the LVM devices file");

  assertTrue($machine->isInLVMDevicesFile($vdoDevicePath),
             "VDO device should still be in the LVM devices file");

  my $vgName = $vdoDevice->{volumeGroup}->getName();
  my $lvName = $vdoDevice->getDeviceName();
  my $fullLVName = "$vgName/$lvName";

  $self->destroyTestDevice($vdoDevice);

  # Scenario 2 - Cleanup after top device removed from stack
  $self->assertLVMDevicesCleanupValid();

  assertFalse($machine->isInLVMDevicesFile($vdoDevicePath),
              "VDO device should not be in the LVM devices file");

  # lvs exits with 5 if the VG or LV is not found
  my $result = $machine->sendCommand("sudo lvs '$fullLVName'");
  assertNe(0, $result, "VDO Device should not be found in lvs after destruction");
  my $stderr = $machine->getStderr();
  my $regexp = qr/[Volume group|Logical volume] ".*" not found/;
  assertRegexpMatches($regexp, $stderr);

  $machine->runSystemCmd("sudo test ! -e " . $vdoDevicePath);

  $self->destroyTestDevice($rawDevice);

  # Scenario 3 - Cleanup after bottom device removed from stack
  $self->assertLVMDevicesCleanupValid();

  # A raw device will still exist in the system after destruction, so it should
  # still be in the LVM devices file.
  assertTrue($machine->isInLVMDevicesFile($rawDevicePath),
             "Raw device should still be in the LVM devices file after destruction");

  $machine->runSystemCmd("sudo test -e " . $rawDevicePath);
}

1;
