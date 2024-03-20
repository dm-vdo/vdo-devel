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
  my $config = $self->getLVMConfig();
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

  my $config = $self->getLVMConfig();
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
  $machine->runSystemCmd("sudo lvcreate --name $name --size ${ksize}K --yes"
                         . " $config $self->{volumeGroup} </dev/null");
  # Make sure to wait for the udev event recording the new event has
  # been processed. Otherwise, the lv may be open due to blkid when
  # we try to disable it below.
  $machine->sendCommand("sudo udevadm settle");
  # XXX As soon as we get rid of Squeeze, on which lvcreate doesn't have the
  # -a flag, we can stop doing two separate commands and use lvcreate -an.
  $self->disableLogicalVolume($name);
}

########################################################################
# Create a thin volume
#
# @param name  Thin volume name
# @param size  Thin volume size in bytes
##
sub createThinVolume {
  my ($self, $name, $size) = assertNumArgs(3, @_);
  my $machine = $self->{storageDevice}->getMachine();
  assertEqualNumeric(0, $size % $KB, "size must be a multiple of 1KB");

  my $config = $self->getLVMConfig();
  $self->createVolumeGroup($machine, $config);

  my $ksize = int($size / $KB);
  my $kfree = int($self->getFreeBytes() / $KB);
  if ($ksize > $kfree) {
    $log->info("requested size ${ksize}K is too large, using ${kfree}K");
    $ksize = $kfree;
  }

  # Create a thin volume in an existing volume group. This is a two step
  # process where we create a thin pool first, then a thin volume.
  # If lvcreate asks for a yes/no on whether to wipeout a filesystem
  # signature (it does on RHEL7), the input redirection will cause the
  # answer to be "no". The volume is not immediately enabled.
  $machine->runSystemCmd("sudo lvcreate --name $name-pool --type=thin-pool"
                         . " --extents 100\%FREE --yes $config"
                         . " $self->{volumeGroup} </dev/null");

  $machine->runSystemCmd("sudo lvcreate --name $name --type=thin"
                         . " --virtualsize=${ksize}K --thinpool=$name-pool"
                         . " --yes $config $self->{volumeGroup}"
                         . " </dev/null");

  # Make sure to wait for the udev event recording the new event has
  # been processed. Otherwise, the lv may be open due to blkid when
  # we try to disable it below.
  $machine->sendCommand("sudo udevadm settle");
  # XXX As soon as we get rid of Squeeze, on which lvcreate doesn't have the
  # -a flag, we can stop doing two separate commands and use lvcreate -an.
  $self->disableLogicalVolume($name);
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

  $config = $self->getLVMConfig($config);
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
  $machine->runSystemCmd("sudo lvcreate $args --yes -ay $config"
                         . " $self->{volumeGroup} </dev/null");

  # Make sure to wait for the udev event recording the new event has
  # been processed. Otherwise, the lv may be open due to blkid when
  # we try to disable it below.
  $machine->sendCommand("sudo udevadm settle");
  # XXX As soon as we get rid of Squeeze, on which lvcreate doesn't have the
  # -a flag, we can stop doing two separate commands and use lvcreate -an.
  $self->disableAutoActivation($name);
  $self->disableLogicalVolume($name);
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
  my $config = $self->getLVMConfig();
  $machine->runSystemCmd("sudo lvremove --force $config $lvPath");
  if (--$self->{_useCount} == 0) {
    $log->info("Automatically removing VG " . $self->{volumeGroup});
    $self->remove();
  }
}

########################################################################
# Delete a logical volume
#
# @param name  Logical volume name
##
sub deleteThinVolume {
  my ($self, $name) = assertNumArgs(2, @_);
  $self->disableLogicalVolume($name);
  # remove a logical volume
  my $machine = $self->{storageDevice}->getMachine();
  my $lvPath = "$self->{volumeGroup}/$name";
  $machine->runSystemCmd("sudo lvremove --force $lvPath");
  $lvPath = "$self->{volumeGroup}/$name-pool";
  $machine->runSystemCmd("sudo lvremove --force $lvPath");
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
  my $config = $self->getLVMConfig();
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
  my $config = $self->getLVMConfig();
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
    my $config  = $self->getLVMConfig();
    $machine->runSystemCmd("sudo vgs -o vg_free --noheadings --units k "
                           . "$config --nosuffix $self->{volumeGroup}");
    my $res = $machine->getStdout();
    $res =~ s/^\s*(.*)\s*$/$1/;
    return int($res) * $KB;
  } else {
    # XXX The intent of this appears to be to round down, but it does nothing.
    return ($self->{storageDevice}->getSize()
            / $self->{physicalExtentSize}
            * $self->{physicalExtentSize});
  }
}

########################################################################
# Return an LVM config argument to use in lvm command. Adds in an
# additional config filter to strip out test devices such as
# Permabit::BlockDevice::TestDevice::Fua in order to prevent LVM
# commands from seeing duplicate PVs.
#
# @oparam config String of config parameters to use
#
# @return the complete config argument for lvm commands
##
sub getLVMConfig {
  my ($self, $config) = assertMinMaxArgs([""], 1, 2, @_);
  my $storageDevice = $self->{storageDevice};
  my $filter = "devices {scan_lvs=1 use_devicesfile=0}";
  if ($storageDevice->isa('Permabit::BlockDevice::TestDevice')) {
    my $underlyingDevice = $storageDevice->getStorageDevice();
    my $underlyingStorage = $underlyingDevice->getDevicePath();
    $filter .= " devices {filter=r|$underlyingStorage|}";
  }
  return "--config '" . $filter . " " . $config . "'";
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
  my $config = $self->getLVMConfig();
  # remove a volume group
  $machine->runSystemCmd("sudo vgremove $config $self->{volumeGroup}");
  # remove a physical volume
  my $storagePath = $storageDevice->getDevicePath();
  $machine->runSystemCmd("sudo pvremove $config $storagePath");
}

1;
