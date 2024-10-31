##
# Perl object that represents a kernel VDO device using a kernel Albireo index.
#
# $Id$
##
package Permabit::BlockDevice::VDO::Unmanaged;

use bignum;
use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Basename;
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertMinMaxArgs
  assertNumArgs
);
use Permabit::CommandString::VDOFormat;
use Permabit::Constants;
use Permabit::Utils qw(parseBytes sizeToLvmText);

use base qw(Permabit::BlockDevice::VDO);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple whether the device has been formatted
     _formatted    => 0,
     # @ple whether to use major/minor specification for underlying device
     useMajorMinor => 0,
     # @ple table version to create. default to current.
     version       => 5,
     # write policy: always async, but required
     _writePolicy  => "async",
    );
##

########################################################################
# @inherit
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2, @_);
  $self->SUPER::configure($arguments);
  assertDefined($self->{logicalSize});
  # We can't just default these in PROPERTIES or INHERITED_PROPERTIES, because
  # the base class sets them to undef in its INHERITED_PROPERTIES and those are
  # the values that VDOTest uses. Hence we have to default them here.
  $self->{blockMapCacheSize} ||= 128 * $MB;
  $self->{blockMapPeriod}    ||= 16380;
}

########################################################################
# Format a block device as a VDO device
#
# @oparam extraArgs    extra arguments for the vdoFormat command object
##
sub formatVDO {
  my ($self, $extraArgs) = assertMinMaxArgs([{}], 1, 2, @_);

  # Sets up the formaatting variables for the table line.
  if ($self->{formatInKernel}) {
    $self->formatVDOInKernel($extraArgs);
    return;
  }

  my $device = $self->{storageDevice};
  my $machine = $device->getMachine();
  my $args = {
              albireoMem    => $self->{memorySize},
              albireoSparse => $self->{sparse},
              binary        => $machine->findNamedExecutable("vdoformat"),
              doSudo        => 1,
              force         => 1,
              logicalSize   => sizeToLvmText($self->{logicalSize}),
              storage       => $device->getSymbolicPath(),
              %$extraArgs,
             };
  # Note device properties are inherited.
  my $vdoFormat = Permabit::CommandString::VDOFormat->new($self, $args);
  $device->getMachine()->assertExecuteCommand("($vdoFormat)");

}

########################################################################
# This is the formatVDO version of formatting in the kernel. The only
# thing it really needs to do is wipe the storage device of VDO
# signatures. However, these functions are also used for testing, so
# in order to test formatting in the kernel, we need to make a fake
# version of the table line from passed in parameters and then start
# and stop the vdo device.
#
# @oparam extraArgs    extra arguments for formatting in the kernel
##
sub formatVDOInKernel {
  my ($self, $extraArgs) = assertMinMaxArgs([{}], 1, 2, @_);

  my $path = $self->getStoragePath();
  if (defined($extraArgs->{force}) && $extraArgs->{force} == 0) {
    $self->runOnHost(["sudo wipefs --all $path"]);
  } else {
    $self->runOnHost(["sudo wipefs --force --all $path"]);
    $self->runOnHost(["sudo dd if=/dev/zero of=$path bs=4096 count=1"]);
  }

  if ($extraArgs) {
    $self->runOnHost(["sudo dmsetup targets",
                      "sudo dmsetup create $self->{deviceName} --table \""
                      . $self->makeConfigString($extraArgs) . '"',
                      "sudo dmsetup status",
                      "sudo dmsetup table $self->{deviceName}",
                      "sudo dmsetup info $self->{deviceName}",
                     ], "\n");
    $self->waitForIndex();
    $self->runOnHost(["sudo dmsetup remove $self->{deviceName}",
                     ], "\n");
  }
}

########################################################################
# Parse passed in logical size string to int value. String may or may
# not contain suffixes representing various sizes (bytes, megabytes, etc).
# If there is no suffix, the value is assumed to be in megabytes, just
# like our vdoformat tool does.
#
# @param bytesStr string representing a size
#
##
sub _parseLogicalSize {
  my ($sizeStr) = assertNumArgs(1, @_);
  my $parse = ($sizeStr =~ m/^([-| ])*([\d\.]+) *(\w)?\w* *$/);
  if ($parse) {
    $log->info("Value = $2");
    my $size = $2;
    if (defined($3)) {
      $log->info("Suffix = $3");
      $size = $size . $3;
    } else {
      $size = $size . "M";
    }
    my $logicalSize = sprintf("%llu", parseBytes($size));
    if (defined($1) && $1 eq "-") {
      $logicalSize = $logicalSize * -1;
    }
    return $logicalSize;
  }
  return 0;
}

########################################################################
# Make the dmsetup table to create this VDO invocation. Include
# support for old table lines to allow for testing of mismatched
# user and kernel modes. The version is determined by a property
# of this object.
#
# @extraArgs extraArgs override values for config string
##
sub makeConfigString {
  my ($self, $extraArgs) = assertMinMaxArgs([{}], 1, 2, @_);

  # If you add a new version, please update the default value
  # in the property list and add a config below.
  my @threadCounts = ();
  if (defined($self->{bioAckThreadCount})) {
    push(@threadCounts, ["ack", "$self->{bioAckThreadCount}"]);
  }
  if (defined($self->{bioThreadCount})) {
    push(@threadCounts, ["bio", "$self->{bioThreadCount}"]);
  }
  if (defined($self->{bioThreadRotationInterval})) {
    push(@threadCounts,
	 ["bioRotationInterval", "$self->{bioThreadRotationInterval}"]);
  }
  if (defined($self->{cpuThreadCount})) {
    push(@threadCounts, ["cpu", "$self->{cpuThreadCount}"]);
  }
  if (defined($self->{hashZoneThreadCount})) {
    push(@threadCounts, ["hash", "$self->{hashZoneThreadCount}"]);
  }
  if (defined($self->{logicalThreadCount})) {
    push(@threadCounts, ["logical", "$self->{logicalThreadCount}"]);
  }
  if (defined($self->{physicalThreadCount})) {
    push(@threadCounts, ["physical", "$self->{physicalThreadCount}"]);
  }

  my $logicalSize
    = defined($extraArgs->{logicalSize})
      ? _parseLogicalSize($extraArgs->{logicalSize})
      : $self->{logicalSize};
  $log->info("logical size = $logicalSize");

  my @optional = ();

  if (defined($self->{enableCompression})) {
    # magic value -1 suppresses the option completely
    if ($self->{enableCompression} != -1) {
      push(@optional,
	   ["compression", $self->{enableCompression} ? "on" : "off"]);
    }
  }

  if (defined($self->{enableDeduplication})) {
    # magic value -1 suppresses the option completely
    if ($self->{enableDeduplication} != -1) {
      push(@optional,
	   ["deduplication", $self->{enableDeduplication} ? "on" : "off"]);
    }
  }

  my $memorySize
    = defined($extraArgs->{albireoMem}) ? $extraArgs->{albireoMem} : $self->{memorySize};
  if (defined($memorySize)) {
    # magic value -1 suppresses the option completely
    if ($memorySize != -1) {
      push(@optional, ["indexMemory", "$memorySize"]);
    }
  }

  my $sparse
    = defined($extraArgs->{albireoSparse}) ? $extraArgs->{albireoSparse} : $self->{sparse};
  if (defined($sparse)) {
    # magic value -1 suppresses the option completely
    if ($sparse != -1) {
      push(@optional,
           ["indexSparse", $sparse ? "on" : "off"]);
    }
  }

  my $slabBits
    = defined($extraArgs->{slabBits}) ? $extraArgs->{slabBits} : $self->{slabBits};
  if (defined($slabBits)) {
    # magic value -1 suppresses the option completely
    if ($slabBits != -1) {
      push(@optional, ["slabBits", "$slabBits"]);
    }
  }

  my $storageSpecification
    = ($self->{useMajorMinor}
       ? join( ":", $self->getStorageDevice()->getDeviceMajorMinor())
       : $self->getStoragePath());

  my %config;
  $config{5} = [
		0,
		int($logicalSize / $SECTOR_SIZE),
		"vdo",
		"V4",
		$storageSpecification,
		int($self->{storageDevice}->getSize() / $self->{blockSize}),
		$self->{emulate512Enabled} ? "512" : $self->{blockSize},
		int($self->{blockMapCacheSize} / $self->{blockSize}),
		$self->{blockMapPeriod},
		join(" ", map { join(" ", @$_) } @threadCounts),
		join(" ", map { join(" ", @$_) } @optional),
	       ];
  $config{4} = [
		0,
		int($self->{logicalSize} / $SECTOR_SIZE),
		"vdo",
		"V4",
		$storageSpecification,
		int($self->{storageDevice}->getSize() / $self->{blockSize}),
		$self->{emulate512Enabled} ? "512" : $self->{blockSize},
		int($self->{blockMapCacheSize} / $self->{blockSize}),
		$self->{blockMapPeriod},
		join(" ", map { join(" ", @$_) } @threadCounts),
	       ];
  $config{3} = [
		0,
		int($self->{logicalSize} / $SECTOR_SIZE),
		"vdo",
		"V3",
		$storageSpecification,
		int($self->{storageDevice}->getSize() / $self->{blockSize}),
		$self->{emulate512Enabled} ? "512" : $self->{blockSize},
		int($self->{blockMapCacheSize} / $self->{blockSize}),
		$self->{blockMapPeriod},
		$self->{_writePolicy},
		join(" ", map { join(" ", @$_) } @threadCounts),
	       ];
  $config{2} = [
		0,
		int($self->{logicalSize} / $SECTOR_SIZE),
		"vdo",
		"V2",
		$storageSpecification,
		int($self->{storageDevice}->getSize() / $self->{blockSize}),
		$self->{emulate512Enabled} ? "512" : $self->{blockSize},
		int($self->{blockMapCacheSize} / $self->{blockSize}),
		$self->{blockMapPeriod},
		"on",
		$self->{_writePolicy},
		defined($extraArgs->{poolName}) ? $extraArgs->{poolName} : $self->{deviceName},
		join(" ", map { join(" ", @$_) } @threadCounts),
	       ];
  if (defined($self->{vdoMaxDiscardSectors})) {
    push(@{$config{4}}, $self->{vdoMaxDiscardSectors} / 8);
    push(@{$config{3}}, $self->{vdoMaxDiscardSectors} / 8);
    push(@{$config{2}}, $self->{vdoMaxDiscardSectors} / 8);
  }

  $config{1} = [
		0,
		int($self->{logicalSize} / $SECTOR_SIZE),
		"vdo",
		"V1",
		$storageSpecification,
		int($self->{storageDevice}->getSize() / $self->{blockSize}),
		$self->{emulate512Enabled} ? "512" : $self->{blockSize},
		"enabled",
		2000,
		int($self->{blockMapCacheSize} / $self->{blockSize}),
		$self->{blockMapPeriod},
		"on",
		$self->{_writePolicy},
		$self->{deviceName},
		join(",", map { join("=", @$_) } @threadCounts) || ".",
	       ];
  $config{0} = [
		0,
		int($self->{logicalSize} / $SECTOR_SIZE),
		"vdo",
		$storageSpecification,
		$self->{emulate512Enabled} ? "512" : $self->{blockSize},
		"disabled",
		0,
		int($self->{blockMapCacheSize} / $self->{blockSize}),
		$self->{blockMapPeriod},
		"on",
		$self->{_writePolicy},
		$self->{deviceName},
		join(",", map { join("=", @$_) } @threadCounts) || ".",
	       ];

  return join(" ", @{$config{$self->{version}}});
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->{_formatted}) {
    $self->formatVDO();
    $self->{_formatted} = 1;
  }

  $self->runOnHost(["sudo dmsetup targets",
                    "sudo dmsetup create $self->{deviceName} --table \""
                    . $self->makeConfigString() . '"',
                    "sudo dmsetup status",
                    "sudo dmsetup table $self->{deviceName}",
                    "sudo dmsetup info $self->{deviceName}"
                   ], "\n");
  $self->addDeactivationStep(sub { $self->stopVDO(); });
  $self->SUPER::activate();
}

########################################################################
# Restarts the device using a table line consistent with the version
#
# @param The version to restart VDO as.
##
sub restartAsVersion {
  my ($self, $version) = assertNumArgs(2, @_);
  assertDefined($version);
  if (!defined($self->{version}) || ($self->{version} != $version)) {
    $self->stop();
    $self->{version} = $version;
    $self->start();
  }
}

########################################################################
# @inherit
##
sub stopVDO {
  my ($self) = assertNumArgs(1, @_);
  $self->getMachine()->dmsetupRemove($self->{deviceName});
}

########################################################################
# Reload the device with a new table.
#
# @oparam okToFail Whether to throw an exception on failure or not.
##
sub _reloadTable {
  my ($self, $okToFail) = assertMinMaxArgs([0], 1, 2, @_);
  my $commands = ["sudo dmsetup reload $self->{deviceName} --table \""
                  . $self->makeConfigString() . '"',
                  "sudo dmsetup resume $self->{deviceName}"];
  if ($okToFail) {
    eval { $self->runOnHost($commands) };
  } else {
    $self->runOnHost($commands);
  }
}

########################################################################
# Rename the device.
#
# @param  newName  The name to rename the VDO device to.
# @oparam okToFail Whether to throw an exception on failure or not.
##
sub _renameTable {
  my ($self, $newName, $okToFail) = assertMinMaxArgs([0], 2, 3, @_);
  my $commands = ["sudo dmsetup reload $self->{deviceName} --table \""
                  . $self->makeConfigString({poolName => $newName}) . '"',
		  "sudo dmsetup rename $self->{deviceName} $newName",
		  "sudo dmsetup resume $newName"];
  if ($okToFail) {
    eval { $self->runOnHost($commands) };
  } else {
    $self->runOnHost($commands);
  }
}

########################################################################
# @inherit
##
sub growLogical {
  my ($self, $logicalSize) = assertNumArgs(2, @_);
  {
    local $self->{logicalSize} = $logicalSize;
    $self->_reloadTable();
  }
  $self->{logicalSize} = $logicalSize;
}

########################################################################
# @inherit
##
sub growPhysical {
  my ($self, $physicalSize) = assertNumArgs(2, @_);
  $self->resizeStorageDevice($physicalSize);
  {
    local $self->{physicalSize} = $physicalSize;
    $self->_reloadTable();
  }
  $self->{physicalSize} = $physicalSize;
}

########################################################################
# Rename a VDO device. Unmanaged can support rename.
#
# @param newName The new name for the VDO device.
##
sub renameVDO {
  my ($self, $newName) = assertNumArgs(2, @_);
  $self->_renameTable($newName);
  $self->setDeviceName($newName);
}


########################################################################
# Sets the compression state through table reload
##
sub setCompressionMode {
  my ($self, $enable) = assertNumArgs(2, @_);
  {
    local $self->{enableCompression} = $enable;
    $self->_reloadTable();
  }
  $self->{enableCompression} = $enable;
}

########################################################################
# Disable the compression on a VDO device
##
sub disableCompression {
  my ($self)  = assertNumArgs(1, @_);
  my $state = $self->isVDOCompressionEnabled();
  if ($state) {
    $self->setCompressionMode(0);
  } else {
    $log->info("compression already disabled");
  }
}

########################################################################
# Enable the compression on a VDO device
##
sub enableCompression {
  my ($self)  = assertNumArgs(1, @_);
  my $state = $self->isVDOCompressionEnabled();
  if (!$state) {
    $self->setCompressionMode(1);
  } else {
    $log->info("compression already enabled");
  }
}

########################################################################
# Sets the deduplication state through table reload
##
sub setDeduplicationMode {
  my ($self, $enable) = assertNumArgs(2, @_);
  {
    local $self->{enableDeduplication} = $enable;
    $self->_reloadTable();
  }
  $self->{enableDeduplication} = $enable;
}

########################################################################
# Disable the deduplication on a VDO device
##
sub disableDeduplication {
  my ($self)  = assertNumArgs(1, @_);
  my $state = $self->isVDODedupeEnabled();
  if ($state) {
    $self->setDeduplicationMode(0);
  } else {
    $log->info("deduplication already disabled");
  }
}

########################################################################
# Enable the deduplication on a VDO device
##
sub enableDeduplication {
  my ($self)  = assertNumArgs(1, @_);
  my $state = $self->isVDODedupeEnabled();
  if (!$state) {
    $self->setDeduplicationMode(1);
    $self->waitForIndex();
  } else {
    $log->info("deduplication already enabled");
  }
}

1;
