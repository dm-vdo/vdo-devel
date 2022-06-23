##
# Basic test of configuring multiple VDO devices
#
# $Id$
##
package VDOTest::MultipleDevices;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Basename qw(basename);
use List::MoreUtils qw(zip);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertEqualNumeric
  assertNe
  assertNumArgs
  assertTrue
);
use Permabit::CommandString::VDOStats;
use Permabit::Constants;
use Permabit::Utils qw(makeFullPath);
use Tie::IxHash;
use YAML;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

# Hash keys in order of the fields of the dmssetup status line. Values
# (where defined) are the expected values of those fields.
tie my %DMSETUP_STATUS_FIELDS, 'Tie::IxHash';
%DMSETUP_STATUS_FIELDS = (
                          offset           => "0",
                          sectors          => undef,
                          type             => "vdo",
                          device           => undef,
                          mode             => "normal",
                          recoveryMode     => "-",
                          indexState       => "online",
                          compressionState => "offline",
                          blocksUsed       => undef,
                          totalBlocks      => undef,
                         );

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a small albireo index
     memorySize => 0.25,
     # @ple The type of VDO devices to create
     vdoDeviceType => "lvmvdo",
    );
##

#############################################################################
# Run lsblk and parse the output to a hash representing the device tree.
#
# @param the remote machine to query
#
# @return a hashref to the parsed lsblk output
##
sub _getDeviceTree {
  my ($machine) = assertNumArgs(1, @_);

  my %tree = (
              # maps lvm name (e.g. vdo1-vdo) to short device name (dm-3)
              device => { },
              # maps short device name to lvm name
              name   => { },
              # maps short device name parent device name
              parent => { },
             );
  $machine->executeCommand("lsblk -l -n -o NAME,KNAME,PKNAME");
  for my $line (split("\n", $machine->getStdout())) {
    my ($name, $device, $parent) = split(" ", $line);
    $tree{device}{$name}   = $device;
    $tree{name}{$device}   = $name;
    $tree{parent}{$device} = $parent;
  }

  return \%tree;
}

#############################################################################
# Check that a block device is the backing store (parent or grandparent
# device) reported by VDO status.
#
# @param tree     a hashref of the parsed lsblk output
# @param vdo      the VDO device (BlockDevice object)
# @param backing  the backing device (BlockDevice object)
# @param path     the backing device path reported by VDO
##
sub _assertBackingDevice {
  my ($tree, $vdo, $backing, $path) = assertNumArgs(4, @_);

  # Check that the lsblk output for the VDO matches our BlockDevice info.
  my $vdoName = $vdo->getVDODeviceResolvedName();
  assertEq($tree->{name}{$vdoName}, $vdo->getVDODeviceName(), "vdo device name");

  # Find the short device names (e.g. dm-4) to decide device equality since
  # the paths might be /dev/dm-4 or /dev/mapper/vdo1-vdo0
  my $expectedName = basename($backing->getDevicePath());
  my $reportedName = basename($backing->getMachine()->resolveSymlink($path));

  # The parent of VDO reported by lsblk should match the vdo status output.
  assertEq($tree->{parent}{$vdoName}, $reportedName, "reported parent device");

  # lvm inserts a -vdata device between the VDO and the logical volume we
  # created as a test device, so check both possibilities.
  assertTrue(($expectedName eq $reportedName)
             || ($expectedName eq $tree->{parent}{$reportedName}),
             "The test backing device '$expectedName' should either be VDO ($vdoName)"
             . " parent device ($expectedName) or the grandparent"
             . "($tree->{parent}{$reportedName})");
}

#############################################################################
# Convert a dmsetup status line from a vdo to a hash with a named fields.
# Does some sanity checking of the values against %DMSETUP_STATUS_FIELDS.
#
# @param statusLine  the dmsetup status line
#
# @return a hashref of parsed status fields
##
sub _validateStatusLine {
  my ($statusLine) = assertNumArgs(1, @_);

  assertDefined($statusLine);
  $log->debug("parsing status line '$statusLine'");

  my @keys = keys(%DMSETUP_STATUS_FIELDS);
  my @values = split(" ", $statusLine);
  assertEqualNumeric(scalar(@keys), scalar(@values));

  # Can do this because DMSETUP_STATUS_FIELDS is an ordered hash.
  my %statusFields = zip(@keys, @values);

  while (my ($key, $expected) = each(%DMSETUP_STATUS_FIELDS)) {
    if (defined($expected)) {
      assertEq($expected, $statusFields{$key}, "status line field '$key'");
    }
  }

  return \%statusFields;
}

#############################################################################
# Check fields from dmsetup status against vdostats output.
##
sub _checkStatusFields {
  my ($status, $stats) = assertNumArgs(2, @_);

  assertEq($status->{mode}, $stats->{"operating mode"}, "mode");

  my $blocksUsed = $stats->{"overhead blocks used"} + $stats->{"data blocks used"};
  assertEqualNumeric($status->{blocksUsed}, $blocksUsed, "blocksUsed");
  assertEqualNumeric($status->{totalBlocks}, $stats->{"physical blocks"}, "totalBlocks");
}

#############################################################################
##
sub testMultiple {
  my ($self) = assertNumArgs(1, @_);
  my $storageDevice = $self->getDevice();
  # split the raw device in half and deduct the size of the albireo index
  my $storageSize = $storageDevice->getSize() / 2;
  # Create the appropriate storage devices for each VDO.
  my @parameters = (storageDevice => $storageDevice,
                    volumeGroup   => $self->createVolumeGroup($storageDevice,
                                                              "dedupevg"),
                   );
  $self->{firstVDOLV}
    = $self->createTestDevice("linear",
                              deviceName  => "firstVDO",
                              lvmSize     => $storageSize,
                              @parameters);
  $self->{secondVDOLV}
    = $self->createTestDevice("linear",
                              deviceName => "secondVDO",
                              @parameters);
  my $device = $self->createTestDevice($self->{vdoDeviceType},
                                       deviceName    => "vdo1",
                                       storageDevice => $self->{firstVDOLV});

  my $machine = $device->getMachine();
  my $fs      = $self->createFileSystem($device);

  my $mountPoint = $fs->getMountDir();
  my $file1      = "$mountPoint/foo1";
  my $file2      = "$mountPoint/foo2";
  my $file3      = "$mountPoint/foo3";

  $machine->runSystemCmd("echo Hello World > $file1");

  my $device2 = $self->createTestDevice($self->{vdoDeviceType},
                                        deviceName    => "vdo2",
                                        storageDevice => $self->{secondVDOLV});

  # Both devices should show up in the dmsetup status output.
  $machine->runSystemCmd("sudo dmsetup status");
  my $statusYAML = YAML::Load($machine->getStdout());

  # Split the status lines into fields and bind them to keys in a hash.
  my $statusFields1 = _validateStatusLine($statusYAML->{$device->getVDODeviceName()});
  my $statusFields2 = _validateStatusLine($statusYAML->{$device2->getVDODeviceName()});

  # The backing devices should show up in the vdo status output,
  # though perhaps with modified names or a level of indirection.
  my $tree = _getDeviceTree($machine);
  _assertBackingDevice($tree, $device,  $self->{firstVDOLV},  $statusFields1->{device});
  _assertBackingDevice($tree, $device2, $self->{secondVDOLV}, $statusFields2->{device});

  # Try specifying the VDOs under two different names.
  my $name1 = $device->getVDODeviceResolvedName();
  my $name2 = $device2->getVDODeviceName();
  my $args = {
              doSudo     => 1,
              deviceName => [$name1, $name2],
	      verbose    => 1,
             };
  my $command = Permabit::CommandString::VDOStats->new($self, $args);
  $machine->assertExecuteCommand("$command");
  my $statsYaml = YAML::Load($machine->getStdout());
  assertDefined($statsYaml->{$name1}, "$name1 stats");
  assertDefined($statsYaml->{$name2}, "$name2 stats");
  _checkStatusFields($statusFields1, $statsYaml->{$name1});
  _checkStatusFields($statusFields2, $statsYaml->{$name2});

  # sysfs should show both devices online
  assertEq("online", $device->getVDODedupeStatus());
  assertEq("online", $device2->getVDODedupeStatus());

  $machine->assertExecuteCommand("sudo vgdisplay");
  $machine->assertExecuteCommand("sudo lvdisplay");

  # Rather than plumb through support for mounting a second file
  # system, let's just write this device directly.
  $device2->ddWrite(if    => "/dev/urandom",
                    count => 400,
                    bs    => $self->{blockSize},
                   );

  $machine->dropCaches();
  $machine->runSystemCmd("cp $file1 $file2");
  $machine->dropCaches();
  assertEq("Hello World\n", $machine->cat($file1));
  assertEq("Hello World\n", $machine->cat($file2));

  $fs->unmount();
  $machine->dropCaches();
  $device2->ddWrite(if    => "/dev/urandom",
                    count => 400,
                    bs    => $self->{blockSize},
                   );
  $device->restart();

  # While we have two active devices, check the handling of multiple devices
  # by vdoStats.
  $self->_checkVDOStats($device, $device2);

  $fs->mount();
  $device2->stop();
  $machine->runSystemCmd("cp $file1 $file3");
  $machine->dropCaches();

  assertEq("Hello World\n", $machine->cat($file2));
  assertEq("Hello World\n", $machine->cat($file3));
}

#############################################################################
# Check that df-style VDOStats output (no volume specified) includes both
# devices and is the same as listing them individually.
##
sub _checkVDOStats {
  my ($self, $device1, $device2) = assertNumArgs(3, @_);

  # Get the df-style output. Both devices should be listed, and it
  # shouldn't matter which device we ask.
  my $dfStats1 = $device1->getHumanVDOStats({ si => 1 }, 1);
  my $dfStats2 = $device2->getHumanVDOStats({ si => 1 }, 1);
  assertEq($dfStats1, $dfStats2);

  # Convert that output into an array of three lines.
  my @dfLines = split("\n", $dfStats1);
  assertEqualNumeric(3, scalar(@dfLines));

  # Get the per-device stats from each device.
  my $stats1 = $device1->getHumanVDOStats({ si => 1 });
  my $stats2 = $device2->getHumanVDOStats({ si => 1 });
  assertNe($stats1, $stats2);

  # Convert that output into arrays of two lines.
  my @statLines1 = split("\n", $stats1);
  assertEqualNumeric(2, scalar(@statLines1));
  my @statLines2 = split("\n", $stats2);
  assertEqualNumeric(2, scalar(@statLines2));

  # Compare header lines and remove them from the arrays.
  assertEq($dfLines[0], shift(@statLines1));
  assertEq(shift(@dfLines), shift(@statLines2));

  # Reassemble the remaining output lines in sorted order and compare them.
  assertEq(join("\n", sort(@statLines1, @statLines2)),
           join("\n", sort(@dfLines)))
}

1;
