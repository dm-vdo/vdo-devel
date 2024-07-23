##
# dmsetup interface testing
#
# $Id$
##
package VDOTest::Dmsetup;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Basename;
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEq
  assertEqualNumeric
  assertEvalErrorMatches
  assertFalse
  assertMinMaxArgs
  assertNe
  assertNumArgs
  assertRegexpMatches
  assertTrue);
use Permabit::Constants;
use Permabit::Utils qw(retryUntilTimeout sizeToLvmText yamlStringToHash);
use YAML;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple Use a VDO device
  deviceType => "vdo",
);
##

###############################################################################
# Test that dmsetup operations status and table return the right info, and that
# dmsetup message at least doesn't crash the machine on an unknown message.
##
sub testBasicOps {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  # Check status output contains underlying device, surrounded by
  # spaces, and reports "online".
  my $storageDev = $device->getStorageDevice()->getDevicePath();
  $self->assert_matches(qr/\s\Q$storageDev\E\s.*\sonline/,
                        $device->getStatus());

  # Check table output matches intended config string.
  assertEq($device->getTable(), $device->makeConfigString() . "\n");

  # Try growing, make sure the config string is updated.
  $device->growLogical($self->{logicalSize} * 2);
  assertEq($device->getTable(), $device->makeConfigString() . "\n");

  # Handle unknown message.
  eval {
    $device->sendMessage("California");
  };
  assertNe("", $EVAL_ERROR, "unknown message failed to generate an error");
}

###############################################################################
# Test with non default slab bits
##
sub propertiesConfigNonDefaultSlab {
  return ( slabBits => 20 );
}

###############################################################################
# Test dmsetup message for displaying config information using a non default
# slab bits value.
#
##
sub testConfigNonDefaultSlab {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $path = $device->getVDODevicePath();

  my $output = $device->runOnHost("sudo dmsetup message $path 0 config");
  $output = "config : " . $output;
  my $yaml = yamlStringToHash($output);
  my $config = $yaml->{"config"};

  assertEq($config->{"physicalSize"}, int($device->{storageDevice}->getSize()));
  assertEq($config->{"logicalSize"}, $device->{logicalSize});
  assertEq($config->{"slabSize"}, 1 << $self->{slabBits});
  assertEq($config->{"index"}->{"memorySize"}, $self->{memorySize});
  assertEq($config->{"index"}->{"isSparse"}, $device->{sparse});
}

###############################################################################
# Test dmsetup messages directed at the UDS index
#
# This test alternates between two types of operations.  The first type is a
# change to the state of the index, which happens by sending a 'dmsetup
# message' to the VDO device.  These changes are framed by assertions about the
# deduplication status of VDO.  The relevant checks are often done by calls to
# the assertDeduplicationOffline or assertDeduplicationOnline methods.
#
# The second type of operation is the writing of a small slice of data to the
# device.  Each time we write the exact same data, but the state of the dedupe
# index affects whether we write a new data block or refer to an existing data
# block.  The relevant checks are done by calls to the _checkIndexEntries
# method (which looks at the number of entries in the dedupe index) and
# _checkStats method (which looks at the dedupe advice stats).
##
sub testIndex {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  $self->{_vdoStats} = $device->getVDOStats();
  $self->_checkIndexEntries(0);

  # Pick a block count that is at least 100 blocks and is a multiple of the
  # page size.
  my $pageSize = $device->getMachine()->getPageSize();
  my $blocksPerPage = int($pageSize / $self->{blockSize});
  assertEqualNumeric($pageSize, $blocksPerPage * $self->{blockSize});
  my $blockCount = (int(99 / $blocksPerPage) + 1) * $blocksPerPage;

  # Go offline from online
  $device->assertDeduplicationOnline();
  $device->sendMessage("index-disable");
  $device->assertDeduplicationOffline();

  # Write a slice without using the index.  There should be no dedupe, and
  # these blocks are not entered into the index.
  my $slice = $self->createSlice(blockCount => $blockCount);
  $slice->write(tag => "Tag", fsync => 1);
  $self->_checkStats();
  $self->_checkIndexEntries(0);

  # Go offline from offline
  $device->assertDeduplicationOffline();
  $device->sendMessage("index-disable");
  $device->assertDeduplicationOffline();

  # Write a slice without using the index.  There should be no dedupe, and
  # these blocks are not entered into the index.
  $slice = $self->createSlice(blockCount => $blockCount,
                              offset     => $blockCount);
  $slice->write(tag => "Tag", fsync => 1);
  $self->_checkStats();
  $self->_checkIndexEntries(0);

  # Go online from offline
  $device->assertDeduplicationOffline();
  $device->sendMessage("index-enable");
  $device->assertDeduplicationOnline();

  # Write a slice using the index.  There should be no dedupe, and these blocks
  # will now be entered into the index.  Must use direct I/O on aarch64.
  $slice = $self->createSlice(blockCount => $blockCount,
                              offset     => 2 * $blockCount);
  $slice->write(tag => "Tag", direct => 1, fsync => 1);
  $self->_checkStats();
  $self->_checkIndexEntries($blockCount);

  # Go online from online
  $device->assertDeduplicationOnline();
  $device->sendMessage("index-enable");
  $device->assertDeduplicationOnline();

  # Write a slice using the index.  There should be dedupe this time.  Must use
  # direct I/O on aarch64.
  $slice = $self->createSlice(blockCount => $blockCount,
                              offset     => 3 * $blockCount);
  $slice->write(tag => "Tag", direct => 1, fsync => 1);
  $self->_checkStats($blockCount);
  $self->_checkIndexEntries($blockCount);

  # Go offline from online
  $device->assertDeduplicationOnline();
  $device->sendMessage("index-disable");
  $device->assertDeduplicationOffline();

  # Write a slice without using the index.  There should be no dedupe, as we do
  # not check the index.
  $slice = $self->createSlice(blockCount => $blockCount,
                              offset     => 4 * $blockCount);
  $slice->write(tag => "Tag", fsync => 1);
  $self->_checkStats();
  $self->_checkIndexEntries($blockCount);

  # Create a new index and go online from offline
  $device->assertDeduplicationOffline();
  $device->sendMessage("index-create");
  $self->_waitForNewOfflineIndex();
  $device->assertDeduplicationOffline();
  $device->sendMessage("index-enable");
  $device->assertDeduplicationOnline();

  # Write a slice using the index.  There should be no dedupe, and these blocks
  # will now be entered into the index.  Must use direct I/O on aarch64.
  $slice = $self->createSlice(blockCount => $blockCount,
                              offset     => 5 * $blockCount);
  $slice->write(tag => "Tag", direct => 1, fsync => 1);
  $self->_checkStats();
  $self->_checkIndexEntries($blockCount);

  # Write a slice using the index.  There should be dedupe this time.  Must use
  # direct I/O on aarch64.
  $slice = $self->createSlice(blockCount => $blockCount,
                              offset     => 6 * $blockCount);
  $slice->write(tag => "Tag", direct => 1, fsync => 1);
  $self->_checkStats($blockCount);
  $self->_checkIndexEntries($blockCount);

  # Close the index
  $device->assertDeduplicationOnline();
  $device->sendMessage("index-close");
  $device->waitForIndex(statusList => [qw(closed)]);
  assertEq("closed", $device->getVDODedupeStatus());

  # Write a slice without using the index.  There should be no dedupe, as we do
  # not check the index when it is closed.
  $slice = $self->createSlice(blockCount => $blockCount,
                              offset     => 7 * $blockCount);
  $slice->write(tag => "Tag", fsync => 1);
  $self->_checkStats();
  $self->_checkIndexEntries(0);

  # Open the index
  assertEq("closed", $device->getVDODedupeStatus());
  $device->sendMessage("index-enable");
  $device->waitForIndex();
  $device->assertDeduplicationOnline();

  # Write a slice using the index.  There should be dedupe this time.  Must use
  # direct I/O on aarch64.
  $slice = $self->createSlice(blockCount => $blockCount,
                              offset     => 8 * $blockCount);
  $slice->write(tag => "Tag", direct => 1, fsync => 1);
  $self->_checkStats($blockCount);
  $self->_checkIndexEntries($blockCount);
}

###############################################################################
# Test that creating a device with more than one VDO defining table lines as
# part of the create is disallowed.
##
sub propertiesMultiVdoDefiningTable {
  return ( deviceType => undef, );
}

sub testMultiVdoDefiningTable {
  my ($self) = assertNumArgs(1, @_);

  # Create a volume group and two linear volumes on the device.
  my $device = $self->getDevice();
  my @parameters = (storageDevice => $device,
                    volumeGroup   => $self->createVolumeGroup($device));
  my %lvs = ();
  $lvs{1} = $self->createTestDevice("linear",
                                    deviceName => "firstVDO",
                                    lvmSize    => $device->getSize() / 2,
                                    @parameters);
  $lvs{2} = $self->createTestDevice("linear",
                                    deviceName => "secondVDO",
                                    @parameters);

  # Create two unmanaged VDOs on top of the linear devices.
  # We do this as the lazy way of getting the table lines we need
  # to attempt the creation with more than one VDO defining table line rather
  # than reconfiguring the device creation path to handle multiple underlying
  # storage devices.
  my ($firstVDO, $secondVDO)
    = map({ $self->createTestDevice("vdo",
                                    deviceName    => "vdo$_",
                                    storageDevice => $lvs{$_});
          } (1..2));

  my @firstVDOTable = split(" ", $firstVDO->getTable());
  $firstVDO->stop();

  my @secondVDOTable = split(" ", $secondVDO->getTable());
  $secondVDO->stop();

  # Make the second table line a continuation of the first, create the table
  # to use and the command to execute.
  $secondVDOTable[0] = $firstVDOTable[1];
  my $multiTable = join("\n",
                        join(" ", @firstVDOTable),
                        join(" ", @secondVDOTable));
  my $command = join("",
                     "sudo dmsetup create failure --table \"",
                     $multiTable, "\"");

  # Execute the command and verify the failure is as expected.
  my $machine = $device->getMachine();
  my $kernelCursor = $machine->getKernelJournalCursor();
  $machine->executeCommand($command);
  assertRegexpMatches(qr/device-mapper: .* failed: Invalid argument/s,
                      $machine->getStderr());
  my $pattern = "device-mapper:.* target type vdo must appear alone in table";
  assertTrue($machine->searchKernelJournalSince($kernelCursor, $pattern));
}

###############################################################################
# Test no optional parameters and make sure it works properly.
##
sub testOptionalParameters {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $deviceName = $device->getDeviceName();

  # Some suites set these values. Turn them off
  # here so no optional parameters are sent to VDO.
  $device->{bioAckThreadCount} = undef;
  $device->{bioThreadCount} = undef;
  $device->{bioThreadRotationInterval} = undef;
  $device->{cpuThreadCount} = undef;
  $device->{hashZoneThreadCount} = undef;
  $device->{logicalThreadCount} = undef;
  $device->{physicalThreadCount} = undef;
  $device->{enableDeduplication} = -1;
  $device->{enableCompression} = -1;

  $device->restart();

  my @arguments = split(" ", $device->getTable());
  assertEqualNumeric(scalar(@arguments), 9);
}

###############################################################################
# Test dmsetup create with all versions
##
sub testVersions {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $deviceName = $device->getDeviceName();
  my $config;

  foreach my $version (0 .. 4) {
    # Test the version
    $device->restartAsVersion($version);
    $config = $device->makeConfigString();
    assertEq($device->getTable(), $config . "\n");
  }
}

#############################################################################
# Properties for grow test.
##
sub propertiesVersionZeroPhysicalGrow {
  my ($self) = assertNumArgs(1, @_);
  return (
          deviceType   => "vdo-linear",
          # @ple Initial physical size in bytes
          physicalSize => 5 * $GB,
          # @ple the number of bits in the VDO slab
          slabBits     => $SLAB_BITS_TINY,
         );
}

###############################################################################
# Check that the number of entries indexed has changed in the expected way.
##
sub _checkIndexEntries {
  my ($self, $entries) = assertNumArgs(2, @_);
  assertEqualNumeric($entries, $self->{_vdoStats}->{"entries indexed"});
}

###############################################################################
# Check that the dedupe stats have changed in the expected way.
#
# @oparam valid  How many "dedupe advice valid" to expect
##
sub _checkStats {
  my ($self, $valid) = assertMinMaxArgs([0], 1, 2, @_);
  my $oldStats = $self->{_vdoStats};
  $self->{_vdoStats} = $self->getDevice()->getVDOStats();
  my $newStats = $self->{_vdoStats};
  assertEqualNumeric($oldStats->{"dedupe advice stale"},
                     $newStats->{"dedupe advice stale"});
  assertEqualNumeric($oldStats->{"dedupe advice timeouts"},
                     $newStats->{"dedupe advice timeouts"});
  assertEqualNumeric($oldStats->{"dedupe advice valid"} + $valid,
                     $newStats->{"dedupe advice valid"});
}

###############################################################################
# Wait for a new index to be created (while offline).
##
sub _waitForNewOfflineIndex {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  # First wait for VDO to stop reporting about the old index.
  my $noEntriesIndexed = sub {
    return $device->getVDOStats()->{"entries indexed"} == 0;
  };
  retryUntilTimeout($noEntriesIndexed, "Index not created", 2 * $MINUTE);
  # Then wait for the device to come back to offline state.
  $device->waitForIndex(statusList => [qw(offline)]);
}

###############################################################################
# Test that reloading an inactive table replaces the table and doesn't crash
# as in BZ 1669960 [VDO-4433].
##
sub testReloadInactiveTable {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $deviceName = $device->getDeviceName();

  my $table = $device->getTable();
  my $status = $device->getStatus();
  chomp($table);
  assertEq($table, $device->makeConfigString());
  assertEq("\n", $device->getTable("--inactive"));
  assertEq("\n", $device->getStatus("--inactive"));

  # Create an inactive table on the active VDO instance.
  $device->runOnHost("sudo dmsetup load $deviceName --table \"$table\"");
  assertEq("$table\n", $device->getTable("--inactive"));
  assertEq($status, $device->getStatus("--inactive"));

  # Replace the inactive table. This won't provoke VDO-4433 since the kernel
  # layer should reference the active table config.
  $device->runOnHost("sudo dmsetup load $deviceName --table \"$table\"");
  assertEq("$table\n", $device->getTable("--inactive"));
  assertEq($status, $device->getStatus("--inactive"));

  # Clear the inactive table, then stop VDO so there will be no active table.
  $device->runOnHost("sudo dmsetup clear $deviceName");
  assertEq("\n", $device->getTable("--inactive"));
  assertEq("\n", $device->getStatus("--inactive"));

  # Re-create the device and load only an inactive table.
  $device->stop();
  $device->runOnHost("sudo dmsetup create -n $deviceName");
  $device->runOnHost("sudo dmsetup load $deviceName --table \"$table\"");
  assertEq("\n", $device->getTable());
  assertEq("\n", $device->getStatus());
  assertEq("$table\n", $device->getTable("--inactive"));
  # The status changes asynchronously when creating a new VDO, so just query
  # to exercise the code path.
  $device->getStatus("--inactive");

  # Replace the inactive table. In VDO-4433, this would destroy the config
  # referenced by the kernel layer and leave a dangling pointer.
  $device->runOnHost("sudo dmsetup load $deviceName --table \"$table\"");
  assertEq("$table\n", $device->getTable("--inactive"));
  # Getting the inactive table status would dereference the dangling pointer
  # and crash.
  $device->getStatus("--inactive");

  # Clear the replacement inactive table, which should leave it with no table.
  # This would also follow the dangling pointer.
  $device->runOnHost("sudo dmsetup clear $deviceName");
  assertEq("\n", $device->getTable("--inactive"));
  assertEq("\n", $device->getStatus("--inactive"));
  assertEq("\n", $device->getTable());
  assertEq("\n", $device->getStatus());

  # Restore a normal instance so the teardown code will be happy.
  $device->runOnHost("sudo dmsetup remove $deviceName");
  $device->start();
}

###############################################################################
# Test that starting two VDOs on the same storage simultaneously doesn't work,
# as in BZ 1725052 [VDO-4691].
##
sub testSameStorage {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  my $deviceName = $device->getDeviceName();
  my $table = $device->getTable();
  $table =~ s/$deviceName/${deviceName}2/g;
  # Try making a device with a slightly different pool name but same device.
  eval {
    $device->runOnHost("sudo dmsetup create ${deviceName} --table \"$table\"");
  };
  assertRegexpMatches(qr/Device or resource busy/, $EVAL_ERROR,
                      "simultaneous storage use failed to generate error");
}

#############################################################################
# Properties for grow test.
##
sub propertiesBackingDeviceSizeMismatch {
  my ($self) = assertNumArgs(1, @_);
  return (
          deviceType   => "vdo-linear",
          # @ple Initial physical size in bytes
          physicalSize => 5 * $GB,
          # @ple the number of bits in the VDO slab
          slabBits     => $SLAB_BITS_TINY,
         );
}

###############################################################################
# Test that VDO refuses to resume instead of resuming and generating errors
# when the backing device is smaller than configured. [BZ 1732922] [VDO-4718]
##
sub testBackingDeviceSizeMismatch {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $deviceName = $device->getDeviceName();
  my $devicePath = $device->getStoragePath();

  my $table = $device->makeConfigString();
  my $size = int($device->{storageDevice}->getSize() / $device->{blockSize});

  # Grow by at least the size of the recovery journal and slab summary, and
  # a slab, in order to guarantee we grow by at least one slab and the RJ
  # and slab summary don't overlap the old locations.
  my $newSize = $size + 32768 + 64 + (1 << $device->{slabBits});
  $table =~ s/ $size / $newSize /;

  $device->runOnHost("sudo dmsetup reload $deviceName --table \"$table\"");
  my $preresumeCursor = $machine->getKernelJournalCursor();
  eval {
    $device->resume();
  };
  assertEvalErrorMatches(qr|resume ioctl on .* failed: Invalid argument|);
  assertFalse($machine->searchKernelJournalSince($preresumeCursor,
						 "attempt to access beyond"
						 . " end of device"));
  # The test will take care of tearing down the device.
}

###############################################################################
# Test that dmsetup doesn't crash when passed in an empty set of parameters.
##
sub testEmptyTable {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $deviceName = $device->getDeviceName();
  my $myString = $device->makeConfigString();

  # Remove everything after the vdo target name.
  $myString =~ s/(\d+ \d+ vdo).*/$1/;

  my $commands = ["sudo dmsetup reload $deviceName --table \""  . $myString . '"',
                  "sudo dmsetup resume $deviceName"];

  my $preresumeCursor = $machine->getKernelJournalCursor();
  eval {
    $device->runOnHost($commands);
  };
  assertEvalErrorMatches(qr| reload ioctl on .* failed: Invalid argument|);
  assertTrue($machine->searchKernelJournalSince($preresumeCursor,
						 "Incorrect number of arguments"));
}

#############################################################################
# Properties for device change test.
##
sub propertiesBackingDeviceChange {
  my ($self) = assertNumArgs(1, @_);
  return (
          deviceType   => "vdo-stripfua-linear",
          # @ple Initial physical size must not be set, as fua devices can't
	  #      resize to accommodate.
	  physicalSize => undef,
         );
}

###############################################################################
# Test that VDO works if its backing device changes, while running, to a
# different device presenting the same data.
##
sub testBackingDeviceChange {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $deviceName = $device->getDeviceName();

  # Write a little data
  my $blockCount = 1000;
  my $slice1 = $self->createSlice(blockCount => $blockCount);
  $slice1->write(tag => "slice1", direct => 1, fsync => 1);

  my $fuaDevice = $device->getStorageDevice();
  my $linearDevice = $fuaDevice->getStorageDevice();

  # Temporarily switch the VDO to treat its linear device as its storage
  $device->{storageDevice} = $linearDevice;

  my $table = $device->makeConfigString();

  $device->runOnHost("sudo dmsetup reload $deviceName --table \"$table\"");
  $device->resume();

  # Write new data, verify old.
  my $slice2 = $self->createSlice(blockCount => $blockCount,
				  offset => $blockCount);
  $slice2->write(tag => "slice1", direct => 1, fsync => 1);
  $slice1->verify();

  # Go back to original device
  $device->{storageDevice} = $fuaDevice;
  $table = $device->makeConfigString();

  $device->runOnHost("sudo dmsetup reload $deviceName --table \"$table\"");
  $device->resume();

  # Verify both datasets.
  $slice1->verify();
  $slice2->verify();
}

1;
