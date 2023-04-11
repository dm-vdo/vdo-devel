##
# Make sure we can dump a VDO, run vdoListMetadata on the normal VDO,
# and run vdoDebugMetadata on the dumpfile.
#
# $Id$
##
package VDOTest::DebugTools;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertEqualNumeric
  assertLENumeric
  assertNe
  assertMinArgs
  assertNotDefined
  assertNumArgs
  assertRegexpMatches
);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks to write
     blockCount => 16000,
     # @ple Use a VDO device
     deviceType => "lvmvdo",
    );
##

#############################################################################
# @inherit
##
sub listSharedFiles {
  my ($self) = assertNumArgs(1, @_);
  return ($self->SUPER::listSharedFiles(),
          "src/c++/vdo/user/corruptPBNRef",
          "src/c++/vdo/user/vdoDebugMetadata",
          "src/c++/vdo/user/vdoDumpBlockMap",
          "src/c++/vdo/user/vdoListMetadata",
         );
}

#############################################################################
# Log and assert the existence of the listed fields of a config hash.
#
# @param name    The name of the hash containing the config fields
# @param config  The hash containing the config fields
# @param indent  Prefix for each log line
# @param keys    One or more names of hash fields to log
##
sub _logConfigFields {
  my ($self, $name, $config, $indent, @keys) = assertMinArgs(5, @_);

  assertDefined($config, "$name fields should exist");
  if ($indent ne "") {
    $log->info("$name:");
  }
  for my $key (@keys) {
    my $value = $config->{$key};
    assertDefined($value, "'$key' $name field should exist");
    $log->info($indent . "$key: $value");
  }
}

#############################################################################
# Log the YAML hash representing the output of vdoDumpConfig
##
sub _logConfig {
  my ($self, $config) = assertNumArgs(2, @_);

  $self->_logConfigFields("config",
                          $config,
                          "",
                          qw(DataRegion
                             IndexRegion
                             Nonce
                             ReleaseVersion
                             UUID));
  $self->_logConfigFields("IndexConfig",
                          $config->{IndexConfig},
                          "    ",
                          qw(memory
                             sparse));
  $self->_logConfigFields("VDOConfig",
                          $config->{VDOConfig},
                          "    ",
                          qw(blockSize
                             logicalBlocks
                             physicalBlocks
                             recoveryJournalSize
                             slabJournalBlocks
                             slabSize));
}

#############################################################################
# Check that running vdoDumpBlockMap --lbn $i returns a plausible value for
# a selection of LBNs.
##
sub _verifySingleLBNDump {
  my ($self, $logicalBlocks) = assertNumArgs(2, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $dumpBlockMap = $self->findBinary("vdoDumpBlockMap");
  my $storagePath = $device->getVDOStoragePath();
  my $lbnOffset = 0;

  if ($device->isa("Permabit::BlockDevice::VDO::LVMManaged")) {
    # LVM grabs 512k/128 blocks of metadata space on top of the VDO volume.
    $lbnOffset = 128;
  }

  # Verify many LBNs that we think are mapped.
  my $lbn = 0;
  while ($lbn < $logicalBlocks) {
    my $trueLBN = $lbn + $lbnOffset;
    $machine->runSystemCmd("sudo $dumpBlockMap --lba $trueLBN $storagePath");
    if ($lbn < $self->{blockCount}) {
      assertRegexpMatches(qr/$trueLBN\tmapped     \t\d+/, $machine->getStdout());
      $lbn += int($self->{blockCount} / 16);
    } else {
      assertRegexpMatches(qr/$trueLBN\tunmapped   \t0/, $machine->getStdout());
      $lbn += int($logicalBlocks / 24);
    }
  }
}

#############################################################################
# Parse a block map dump from vdoBlockMap. Tests the correctness of the dump
# indirectly by verifying the topology of the tree described by the entries
# and performing sanity-checks on the values in the dump.
#
# @param dumpOutput    A string containing the entire vdoDumpBlockMap output
# @param firstRootPBN  The PBN of the first block map root
##
sub _parseBlockMapDump {
  my ($self, $dumpOutput, $firstRootPBN) = assertNumArgs(3, @_);

  my $pattern = join(" ",
                     'PBN (\d+)',
                     'slot (\d+)',
                     'height (\d+)',
                     '->',
                     'PBN (\d+)',
                     '\(compression state (\d+)\)');
  my $regexp = qr/$pattern/;

  # A map from PBN to either an integer or an arrayref of mapped PBNs indexed
  # by slot number. The integer is the initial value of the mapping, the
  # expected depth of the node, created when a reference to the node is first
  # seen.
  my %nodes;
  # A map from data block PBN to the reference count for that block.
  my %leaves;
  # An array of root node PBNs.
  my @rootPBNs;
  my $lbnCount = 0;

  open(my $dump, "<", \$dumpOutput);
  while (my $line = <$dump>) {
    my $blame = "(at line $INPUT_LINE_NUMBER: $line)";

    # We got both kinds of whitespace here.
    $line =~ tr/ \t/ /s;

    my ($pbn, $slot, $height, $mappedPBN, $state) = ($line =~ m/$regexp/);
    assertDefined($pbn, "regexp didn't match $blame");

    assertLENumeric($slot, 812, "valid block map page slot $blame");
    assertLENumeric($height, 5, "valid block map tree height $blame");
    assertLENumeric($state, 1, "only uncompressed blocks used $blame");

    # We won't see references to roots, so treat the first entry we see in
    # each one as an implicit reference.
    if (($height == 4)  && ($slot == 0)) {
      push(@rootPBNs, $pbn);
      assertNotDefined($nodes{$pbn}, "unique root PBN $blame");
      $nodes{$pbn} = $height;
    }

    my $node = $nodes{$pbn};
    assertDefined($node, "unknown node PBN $blame");
    assertNotDefined($leaves{$pbn}, "nodes are not data block leaves $blame");

    # If $node is scalar, this is the first entry in a node. Check the height
    # (the scalar value), then replace it with an arrayref for the slots.
    if (!ref($node)) {
      assertEqualNumeric($height, $node, "expected node height $blame");
      $node = [];
      $nodes{$pbn} = $node;
    }

    assertNotDefined($node->[$slot], "unique slots $blame");
    $node->[$slot] = $mappedPBN;

    if ($height > 0) {
      assertNotDefined($nodes{$mappedPBN}, "unique tree nodes $blame");
      # Record the expected height to check when the node is visited.
      $nodes{$mappedPBN} = $height - 1;
    } else {
      $lbnCount++;
      assertNotDefined($nodes{$mappedPBN}, "leaves are not nodes $blame");
      # Count data block references
      $leaves{$mappedPBN} //= 0;
      $leaves{$mappedPBN}++;
      assertLENumeric($leaves{$mappedPBN}, 254,
                      "reference count for $mappedPBN $blame");
    }
  }

  assertEqualNumeric($self->{blockCount}, $lbnCount, "found every LBN used");
  assertEqualNumeric($firstRootPBN, $rootPBNs[0], "first root PBN");
  assertEqualNumeric(scalar(@rootPBNs), $rootPBNs[-1] - $rootPBNs[0] + 1,
                     "contiguous roots");
}

#############################################################################
# Corrupt a VDO. Dump its metadata. List its metadata. Make sure the dumpfile
# is usable.
##
sub testTools {
  my ($self) = assertNumArgs(1, @_);

  my $slice = $self->createSlice(blockCount => $self->{blockCount});
  $slice->write(tag => "Tools", dedupe => 0.9, direct => 1);

  my $device      = $self->getDevice();
  my $stats       = $device->getVDOStats();
  my $machine     = $device->getMachine();
  my $deviceName  = $device->getVDODeviceName();
  my $storagePath = $device->getVDOStoragePath();
  $device->stop();
  $device->enableReadableStorage();

  # At least exercise vdoDumpConfig to make sure it runs and returns expected keys.
  my $config = $device->dumpConfig();
  $self->_logConfig($config);

  # At least check that vdosetuuid changes the UUID.
  $device->setUUID();
  $device->enableReadableStorage();
  assertNe("UUID should have changed",
           $config->{UUID},
           $device->dumpConfig()->{UUID});

  $self->_verifySingleLBNDump($stats->{"logical blocks"});

  # Dump the entire block map before corrupting a PBN. Verify it later after
  # we have the root PBN location from vdoListMetadata.
  my $dumpBlockMap = $self->findBinary("vdoDumpBlockMap");
  my $result = $machine->runSystemCmd("sudo $dumpBlockMap $storagePath");
  my $blockMapDump = $machine->getStdout();

  # Corrupt a PBN.
  $device->disableReadableStorage();
  $device->corruptPBNRef();

  # Try dumping the metadata.
  $device->dumpMetadata("test");

  $device->enableReadableStorage();

  # Try listing the metadata.
  my $listMetadata = $self->findBinary("vdoListMetadata");
  $machine->runSystemCmd("sudo $listMetadata $storagePath");
  $machine->getStdout() =~ qr/(\d+) .. (\d+): block map tree roots/;
  my $firstRootPBN = $1;
  # First data PBN is right after the block map roots.
  my $legitimatePBN = $2 + 1;

  # And examining it with vdoDebugMetadata.
  my $debugMetadata = $self->findBinary("vdoDebugMetadata");
  # Expand the gzipped metadata
  $machine->runSystemCmd("sudo gunzip $self->{runDir}/$deviceName-test");
  $machine->runSystemCmd("sudo $debugMetadata --pbn $legitimatePBN" .
                           " $self->{runDir}/$deviceName-test" .
                           " > $self->{runDir}/$deviceName-debug");

  # Verify the block map dump we captured earlier.
  $self->_parseBlockMapDump($blockMapDump, $firstRootPBN);

  $device->disableReadableStorage();
}

#############################################################################
# Mark the LVMVDO volume as read only.
##
sub markVDOReadOnly {
  my ($self) = assertNumArgs(1, @_);

  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  my $vdoDevicePath = "/dev/mapper/" . $device->{volumeGroup}->getName() .
                      "-vdo0";

  # Mark the volume as read only.
  my $vdoReadOnly = $self->findBinary("vdoReadOnly");
  $machine->runSystemCmd("sudo $vdoReadOnly $vdoDevicePath");
}

#############################################################################
# Modify the LVM stack to allow for a R/W backing store.
##
sub runAdaptLVMScript {
  my ($self, $operation) = assertNumArgs(2, @_);

  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  my $vdoDeviceName = $device->{volumeGroup}->getName() . "/" .
                      $device->{deviceName};

  my $shellUtility = $self->findBinary("adaptLVMVDO.sh");

  # The EXTRA_LVM_ARGS are needed for our environment, but in a typical default
  # LVM configuration environment, they won't be needed.
  my $cmdBase = "sudo EXTRA_LVM_ARGS='-K --config devices/scan_lvs=1' " .
                "$shellUtility";

  # Switch the backing store to r/w so we can work with it.
  $machine->runSystemCmd("$cmdBase $operation $vdoDeviceName");
}

#############################################################################
# Test the ability to mark an LVMVDO volume as read/write and back to read/only
# to allow our tools to function.
#
# Override the device type to lvmvdo since this only applies to that stack.
##
sub propertiesShell {
  return { deviceType => "lvmvdo" };
}

#############################################################################
# Create an LVMVDO stack, tweak the stack to allow RW operations on the backing
# store, mark the VDO volume read-only, reset the stack, and then confirm that
# the VDO is operating in read-only mode.
##
sub testShell {
  my ($self) = assertNumArgs(1, @_);

  $self->runAdaptLVMScript("setrw");

  $self->markVDOReadOnly();

  # Switch back to running in the normal mode where backing store is r/o.
  $self->runAdaptLVMScript("setro");

  assertEq('read-only', $self->getDevice()->getVDOStats()->{"operating mode"});
}

1;
