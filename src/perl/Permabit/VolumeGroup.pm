##
# Perl object that represents an LVM Volume Group
#
# This object provides support for creating and deleting
# both linear and thin type volumes. There are separate
# functions for both types and should not be mixed.
#
# $Id$
##
package Permabit::VolumeGroup;

use strict;
use warnings FATAL => qw(all);
use Carp qw(confess);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEqualNumeric
  assertMinMaxArgs
  assertNumArgs
  assertTrue
  assertType
);
use Permabit::Constants;
use Permabit::Utils qw(retryUntilTimeout);
use POSIX qw(ceil);
use Storable qw(dclone);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
my %PROPERTIES
  = (
     # @ple the size of physical extents in the volume group
     physicalExtentSize => 4 * $MB,
     # @ple storage device BlockDevice object for the volume group
     storageDevice      => undef,
     # @ple the volume group name
     volumeGroup        => "vdo",
     # @ple usage counter
     _useCount          => 0,
);
##

########################################################################
# Creates a C<Permabit::VolumeGroup>.
#
# @params{new}
#
# @return a new C<Permabit::VolumeGroup>
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self = bless {%{ dclone(\%PROPERTIES) },
                    # Overrides previous values
                    @_ }, $class;

  assertType("Permabit::BlockDevice", $self->{storageDevice});
  return $self;
}

########################################################################
# Create a volume group using config settings. This should be called
# for any logical volume create function.
#
# @param machine The machine to run the commands on
# @param config  The LVM config info to run commands with
##
sub createVolumeGroup {
  my ($self, $machine, $config) = assertNumArgs(3, @_);

  if ($self->{_useCount} == 0) {
    $machine->runSystemCmd("sudo modprobe dm_mod");
    # Initialize a disk or partition for use by LVM.
    my $storagePath = $self->{storageDevice}->getDevicePath();
    $machine->runSystemCmd("sudo pvcreate -f -y $config $storagePath");
    # Create a volume group.
    my $ksize = $self->{physicalExtentSize} / $KB;
    $machine->runSystemCmd("sudo vgcreate --physicalextentsize ${ksize}k"
                           . " $config $self->{volumeGroup}"
                           . " $storagePath");
  }
  ++$self->{_useCount};
}

########################################################################
# Change a logical volume
#
# @param  name     Logical volume name
# @param  value    The value to pass to lvchange
##
sub _changeLogicalVolume {
  my ($self, $name, $value) = assertNumArgs(3, @_);
  my $machine = $self->{storageDevice}->getMachine();
  my $lvPath = "$self->{volumeGroup}/$name";

  # Change a logical volume.  The /sbin/udevd daemon can be temporarily
  # accessing the backing device, so we may need to run the command again.
  my $config = $self->getLVMOptions();
  my $changeCommand = "sudo lvchange $value --yes $config $lvPath";

  $log->debug($machine->getName() . ": $changeCommand");
  my $change = sub {
    if ($machine->sendCommand($changeCommand) == 0) {
      return 1;
    }
    my $stderr = $machine->getStderr();
    chomp($stderr);
    # Only deactivation can fail this way.
    if ($stderr !~ m/ in use: not deactivating/) {
      confess("$changeCommand failed:  $stderr");
    }
    return 0;
  };
  retryUntilTimeout($change, "cannot change $lvPath", 10, 0.001);
  # Make sure to wait for the udev event recording the new event has
  # been processed. Otherwise, the lv's node in /dev may not exist
  # when this function returns.
  $machine->sendCommand("sudo udevadm settle");
}

########################################################################
# Create a logical volume
#
# @param name  Logical volume name
# @param size  Logical volume size in bytes
##
sub createLogicalVolume {
  my ($self, $name, $size) = assertNumArgs(3, @_);
  my $machine = $self->{storageDevice}->getMachine();
  assertEqualNumeric(0, $size % $KB, "size must be a multiple of 1KB");

  my $config = $self->getLVMOptions();
  $self->createVolumeGroup($machine, $config);

  my $ksize = int($size / $KB);
  my $kfree = int($self->getFreeBytes() / $KB);
  if ($ksize > $kfree) {
    $log->info("requested size ${ksize}K is too large, using ${kfree}K");
    $ksize = $kfree;
  }

  # Create a logical volume in an existing volume group.  If lvcreate asks for
  # a yes/no on whether to wipeout a filesystem signature (it does on RHEL7),
  # the input redirection will cause the answer to be "no". The volume is not
  # immediately enabled.
  $machine->runSystemCmd("sudo lvcreate --name $name -an --size ${ksize}K --yes"
                         . " $config $self->{volumeGroup} </dev/null");
}

########################################################################
# Create a thin pool
#
# @param  name    Thin pool name
# @param  size    Thin pool size in bytes
# @oparam config  String of create time lvm.conf overrides
# @oparam useVdo  Whether the thin pool uses VDO or not
##
sub createThinPool {
  my ($self, $name, $size, $config, $useVdo) = assertMinMaxArgs(["", 0], 3, 5, @_);
  my $machine = $self->{storageDevice}->getMachine();
  assertEqualNumeric(0, $size % $KB, "size must be a multiple of 1KB");

  $config = $self->getLVMOptions($config);
  $self->createVolumeGroup($machine, $config);

  my $ksize = int($size / $KB);
  my $kfree = int($self->getFreeBytes() / $KB);

  # The first time a thin pool is created, lvm will create both a metadata device
  # and a spare metadata LV in the VG. The spare device behavior can be controlled with
  # the option --poolmetadataspare y|n. Unfortunately, if we try to create a pool using
  # --poolmetadataspare n, the output will still generate a WARNING to stderr, thus
  # causing our perl code to fail.
  #
  # As such, we have to take in account the size of both the metadata device and the
  # spare metadata device when calculating how much space the pool takes as it doesn't
  # take that into account itself. So we take the overall size asked and add the size
  # needed for both the metadata and spare metadata devices. Then we see if the VG
  # has sufficient space. Currently that space is 30 extents for each device.
  my $metaDataSize = (60 * $self->{physicalExtentSize}) / $KB;
  my $fullSize = $ksize + $metaDataSize;

  if ($fullSize > $kfree) {
    my $newSize = $kfree - $metaDataSize;
    $log->info("requested size ${ksize}K plus metadata is too large for VG, using ${newSize}K");
    $ksize = $newSize;
  }

  my $dataType = "";
  if ($useVdo) {
    $dataType = "--pooldatavdo y";
  }

  # Create a thin pool in an existing volume group. The pool is not
  # immediately enabled.
  $machine->runSystemCmd("sudo lvcreate --name $name $dataType --type=thin-pool"
                         . " --size ${ksize}K -an -kn --yes $config"
                         . " $self->{volumeGroup} </dev/null");
}

########################################################################
# Create a thin volume on top of a thin pool
#
# @param pool  The thin pool device to create on top of
# @param name  Thin volume name
# @param size  Thin volume size in bytes
##
sub createThinVolume {
  my ($self, $pool, $name, $size) = assertNumArgs(4, @_);
  my $machine = $self->{storageDevice}->getMachine();
  my $poolName = $pool->getDeviceName();
  assertEqualNumeric(0, $size % $KB, "size must be a multiple of 1KB");
  assertTrue($pool->isa("Permabit::BlockDevice::LVM::ThinPool"));
  assertTrue($self->{_useCount} > 0);

  my $ksize = int($size / $KB);
  ++$self->{_useCount};

  my $config = $self->getLVMOptions();
  # Create a thin volume in an existing thin pool. The volume is not
  # immediately enabled.
  $machine->runSystemCmd("sudo lvcreate --name $name -an --type=thin"
                         . " --virtualsize=${ksize}K --thinpool=$poolName"
                         . " --yes $config $self->{volumeGroup}"
                         . " </dev/null");
}

########################################################################
# Create a vdo volume
#
# @param  name          VDO volume name
# @param  pool          VDO pool name
# @param  physicalSize  VDO physical size in bytes
# @param  logicalSize   VDO logical size in bytes
# @oparam config        String of create time lvm.conf overrides
##
sub createVDOVolume {
  my ($self, $name, $pool, $physicalSize, $logicalSize, $config)
    = assertMinMaxArgs([""], 5, 6, @_);
  my $machine = $self->{storageDevice}->getMachine();
  assertEqualNumeric(0, $physicalSize % $KB,
                     "physical size must be a multiple of 1KB");
  assertEqualNumeric(0, $logicalSize % $KB,
                     "logical size must be a multiple of 1KB");

  $config = $self->getLVMOptions($config);
  $self->createVolumeGroup($machine, $config);

  my $logicalKB = int($logicalSize / $KB);
  my $physicalKB = int($physicalSize / $KB);
  my $physicalFreeKB = int($self->getFreeBytes() / $KB);
  if ($physicalKB > $physicalFreeKB) {
    $log->info("requested size ${physicalKB}K is too large,"
               . " using ${physicalFreeKB}K");
    $physicalKB = $physicalFreeKB;
  }

  # Create a lvm managed VDO in an existing volume group. This will also
  # automatically create a single linear device that sits on top of the VDO.
  # If lvcreate asks for a yes/no on whether to wipeout a filesystem signature
  # (it does on RHEL7), the input redirection will cause the answer to be "no".
  # The volume is not immediately enabled.
  #
  # NB: The name specified here will be the name of the single linear volume
  # on top of the VDO "pool". There appears to be no way currently to set
  # the name of the VDO itself. LVM creates the VDO with the name "vpool<#>"
  # We're using a name of _pool vs -pool here because the /dev/mapper name
  # would end up adding an extra - because it adds the volume group to the
  # name. Example: vdo0-pool would end up being /dev/mapper/<vg>-vdo0--pool.
  my %args = (name => $name,
              vdopool => $pool,
              size => "${physicalKB}K");
  if ($logicalKB > 0) {
    $args{virtualsize} = "${logicalKB}K";
  }

  my $args = join(' ', map { "--$_ $args{$_}" } keys(%args));
  $machine->runSystemCmd("sudo lvcreate $args -an -kn --yes $config"
                         . " $self->{volumeGroup} </dev/null");
}

########################################################################
# Delete a logical volume
#
# @param name  Logical volume name
##
sub deleteLogicalVolume {
  my ($self, $name) = assertNumArgs(2, @_);
  $self->disableLogicalVolume($name);
  # remove a logical volume
  my $machine = $self->{storageDevice}->getMachine();
  my $lvPath = "$self->{volumeGroup}/$name";
  my $config = $self->getLVMOptions();
  $machine->runSystemCmd("sudo lvremove --force $config $lvPath");
  if (--$self->{_useCount} == 0) {
    $log->info("Automatically removing VG " . $self->{volumeGroup});
    $self->remove();
  }
}

########################################################################
# Disable auto activation of logical volume
#
# @param name  Logical volume name
##
sub disableAutoActivation {
  my ($self, $name) = assertNumArgs(2, @_);
  $self->_changeLogicalVolume($name, '-ky');
}

########################################################################
# Disable a logical volume
#
# @param name  Logical volume name
##
sub disableLogicalVolume {
  my ($self, $name) = assertNumArgs(2, @_);
  $self->_changeLogicalVolume($name, '-an');
}

########################################################################
# Enable auto activation of logical volume
#
# @param name  Logical volume name
##
sub enableAutoActivation {
  my ($self, $name) = assertNumArgs(2, @_);
  $self->_changeLogicalVolume($name, '-kn');
}

########################################################################
# Enable a logical volume
#
# @param name  Logical volume name
##
sub enableLogicalVolume {
  my ($self, $name) = assertNumArgs(2, @_);
  $self->_changeLogicalVolume($name, '-ay -K');
}

########################################################################
# Change the size of a logical volume using the specified lvm program.
#
# @param command  One of lvresize, lvextend, or lvreduce (and any extra args)
# @param name     Logical volume name
# @param newSize  New logical volume size in bytes
##
sub _changeLogicalVolumeSize {
  my ($self, $command, $name, $newSize) = assertNumArgs(4, @_);
  my $machine = $self->{storageDevice}->getMachine();
  assertEqualNumeric(0, $newSize % $self->{physicalExtentSize},
                     "size must be a multiple of the physical extent size");
  my $sizeKB = $newSize / $KB;
  my $config = $self->getLVMOptions();
  my $path   = "$self->{volumeGroup}/$name";
  $machine->runSystemCmd("sudo $command --size ${sizeKB}k $config $path");
}

########################################################################
# Change the size of a logical volume
#
# @param name     Logical volume name
# @param newSize  New logical volume size in bytes
##
sub resizeLogicalVolume {
  my ($self, $name, $newSize) = assertNumArgs(3, @_);
  $self->_changeLogicalVolumeSize("lvresize --force", $name, $newSize);
}

########################################################################
# Extend the size of a logical volume
#
# @param name     Logical volume name
# @param newSize  New logical volume size in bytes
##
sub extendLogicalVolume {
  my ($self, $name, $newSize) = assertNumArgs(3, @_);
  $self->_changeLogicalVolumeSize("lvextend", $name, $newSize);
}

########################################################################
# Reduce the size of a logical volume
#
# @param name     Logical volume name
# @param newSize  New logical volume size in bytes
##
sub reduceLogicalVolume {
  my ($self, $name, $newSize) = assertNumArgs(3, @_);
  $self->_changeLogicalVolumeSize("lvreduce --force", $name, $newSize);
}

########################################################################
# Rename the logical volume
#
# @param name     Logical volume name
# @param newName  New logical volume name
##
sub renameLogicalVolume {
  my ($self, $name, $newName) = assertNumArgs(3, @_);
  my $machine = $self->{storageDevice}->getMachine();
  my $config = $self->getLVMOptions();
  $machine->runSystemCmd("sudo lvrename $config"
			 . " $self->{volumeGroup}/$name"
			 . " $self->{volumeGroup}/$newName");
}

########################################################################
# Round a size up to a multiple of the physical extent size.
#
# @param size  The number of bytes to make aligned
#
# @return  The new size that is aligned for this device.
##
sub alignToExtent {
  my ($self, $size) = assertNumArgs(2, @_);
  my $extentsNeeded = ceil($size / $self->{physicalExtentSize});
  my $alignedSize = $extentsNeeded * $self->{physicalExtentSize};
  return $alignedSize;
}

########################################################################
# Gets the free space in the volume group in bytes
##
sub getFreeBytes {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{_useCount} > 0) {
    my $machine = $self->{storageDevice}->getMachine();
    my $config  = $self->getLVMOptions();
    $machine->runSystemCmd("sudo vgs -o vg_free --noheadings --units k "
                           . "$config --nosuffix $self->{volumeGroup}");
    my $result = $machine->getStdout();
    $result =~ s/^\s*(.*)\s*$/$1/;
    return int($result) * $KB;
  } else {
    # XXX The intent of this appears to be to round down, but it does nothing.
    return ($self->{storageDevice}->getSize()
            / $self->{physicalExtentSize}
            * $self->{physicalExtentSize});
  }
}

########################################################################
# Return a --devices string to use in an lvm command. If the storage
# device for the volume group is a TestDevice, we will filter out the
# TestDevice's storage device from the list so we can avoid duplicate
# PVs.
#
# @return the devices parameter string to use
##
sub getLVMDevices {
  my ($self) = assertNumArgs(1, @_);
  my $storageDevice = $self->{storageDevice};
  my $machine = $storageDevice->getMachine();
  if ($storageDevice->isa('Permabit::BlockDevice::TestDevice')) {
    # Create a copy of the devices file and filter it
    my $underlyingDevice = $storageDevice->getStorageDevice();
    my $underlyingStorage = $underlyingDevice->getDevicePath();

    my @validDevices = $machine->getValidDevicesInLVMDevicesFile();
    my @filtered = ();
    foreach my $deviceInfo (@validDevices) {
      if ($underlyingStorage !~ /$deviceInfo->{resolvedName}/) {
        push(@filtered, $deviceInfo->{deviceName});
      }
    }
    if (scalar(@filtered) > 0) {
      return "--devices '" . join(",", @filtered) . "'";
    }
  }
  return "";
}

########################################################################
# Return an LVM config argument to use in an lvm command. Add an
# additional option to make sure LVM allows PVs on top of PVs.
#
# @param config String of config parameters to add scan_lvs to
#
# @return the complete config argument for lvm commands
##
sub getLVMConfig {
  my ($self, $config) = assertNumArgs(2, @_);
  my $fullConfig = "$config devices/scan_lvs=1";
  return "--config '" . $fullConfig . "'";
}

########################################################################
# Return both --config and --devices parameters for lvm commands to use.
#
# @return the combination of --config and --devices parameters
##
sub getLVMOptions {
  my ($self, $config) = assertMinMaxArgs([""], 1, 2, @_);
  return $self->getLVMConfig($config) . " " . $self->getLVMDevices();
}

########################################################################
# Get the machine containing the logical volume
#
# @return the machine
##
sub getMachine {
  my ($self) = assertNumArgs(1, @_);
  return $self->{storageDevice}->getMachine();
}

########################################################################
# Get the name of the logical volume
#
# @return the name
##
sub getName {
  my ($self) = assertNumArgs(1, @_);
  return $self->{volumeGroup};
}

########################################################################
# Remove the volume group
##
sub remove {
  my ($self) = assertNumArgs(1, @_);
  my $storageDevice = $self->{storageDevice};
  my $machine = $storageDevice->getMachine();
  my $config = $self->getLVMOptions();
  # remove a volume group
  $machine->runSystemCmd("sudo vgremove $config $self->{volumeGroup}");
  # remove a physical volume
  my $storagePath = $storageDevice->getDevicePath();
  $machine->runSystemCmd("sudo pvremove $config $storagePath");
}

1;
