##
# Base class for VDOTests.
#
# $Id$
##
package VDOTest;

use strict;
use warnings FATAL => qw(all);
use Carp qw(confess croak);
use Cwd qw(cwd);
use English qw(-no_match_vars);
use File::Path;
use List::Util qw(max);
use Permabit::AlbireoProfilingCommand;
use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertFalse
  assertGTNumeric
  assertMinArgs
  assertMinMaxArgs
  assertNotDefined
  assertNumArgs
  assertTrue
  assertType
);
use Permabit::BlockDevice::VDO;
use Permabit::Constants;
use Permabit::FileSystem::Ext3;
use Permabit::FileSystem::Ext4;
use Permabit::FileSystem::Nfs;
use Permabit::FileSystem::Xfs;
use Permabit::GenSlice;
use Permabit::KernelUtils qw(
  setupKernelMemoryLimiting
  removeKernelMemoryLimiting
  setupRawhideKernel
  removeRawhideKernel
);
use Permabit::PlatformUtils qw(getDistroInfo);
use Permabit::RSVPer;
use Permabit::StorageStack;
use Permabit::SystemUtils qw(
  assertCommand
  assertSystem
  createRemoteFile
);
use Permabit::UserMachine;
use Permabit::Utils qw(
  makeFullPath
  parseBytes
  secondsToMS
  sizeToText
);
use Permabit::Version qw($VDO_MARKETING_VERSION $VDO_MODNAME $VDO_VERSION);
use Permabit::VolumeGroup;

use base qw(Permabit::Testcase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

# XXX This should be computed from device parameters and verified
# against reality. For now, it's hard-coded with the value
# appropriate for LowMemGenData* tests as currently configured.
my $VDO_REQUIRED_MEMORY = 437 * $MB;

# Common Properties
our %PROPERTIES
  = (
     # All the inheritable properties used by any AlbireoProfilingCommand
     %Permabit::AlbireoProfilingCommand::INHERITED_PROPERTIES,
     # All the inheritable properties used by any BlockDevice::VDO
     %Permabit::BlockDevice::VDO::BLOCKDEVICE_INHERITED_PROPERTIES,
     # audit the VDO before tear-down
     auditVDO                => 0,
     # a comma separated list of device types for which to enable block
     # tracing
     blockTrace              => undef,
     # @ple what class of machine to run the test on
     clientClass             => "VDO,FARM",
     # @ple Label for the client host
     clientLabel             => "vdo",
     # @ple The names of the machines to be used for clients.  If not
     #      specified, numClients machines will be reserved.
     clientNames             => undef,
     # @ple chunk size in bytes in which compressible data is written
     compressibleChunkSize   => 4 * $KB,
     # @ple percentage of compressibility to use when creating data files
     compressibility         => 0,
     # @ple Optional default host to use for the test
     defaultHost             => undef,
     # @ple if defined, set up this type of device for the test
     deviceType              => undef,
     # @ple options for the dory device (if any)
     doryOptions             => {},
     # @ple the block size for the filesystem (defaults to VDO exported size)
     fsBlockSize             => undef,
     # @ple the type of filesystem to create
     fsType                  => "xfs",
     # @ple the max number of hung task warnings to report in kern.log
     hungTaskWarnings        => 25,
     # @ple the extent size of the logical volume
     logicalVolumeExtentSize => undef,
     # @ple physical size of the underlying storage (may include a suffix),
     #      used for setting up a loop device
     loopSize                => undef,
     # @ple Limit the kernel memory based on test configuration
     lowMemoryTest           => 0,
     # @ple size of the storage volume (may include a suffix) used for
     #      deviceType "linear" and possibly other LVM type volumes.
     lvmSize                 => undef,
     # @ple The RSVP class nfs clients should be reserved from
     nfsclientClass          => undef,
     # @ple Label for the nfsclient host
     nfsclientLabel          => "nfs",
     # @ple The names of the machines to be used for nfs clients.  If not
     #      specified and an NFS filesystem is needed, numNfsclients
     #      machines will be reserved.
     nfsclientNames          => [],
     # @ple use one client machine
     numClients              => 1,
     # @ple number of NFS client machines to use.  This value will
     #      automatically be increased if the fsType specifies an NFS
     #      filesystem.
     numNfsclients           => 0,
     # @ple Reference to the list of pre-reserved hosts passed in to the test.
     prereservedHosts        => [],
     # @ple physical size of the underlying storage (may include a suffix),
     #      used for setting up a logical volume for VDO using LVM
     physicalSize            => undef,
     # @ple Ask rsvpd to randomize its list of available hosts before selecting
     randomizeReservations   => 1,
     # @ple Run the test on machines with the latest rawhide kernel installed
     rawhideKernel           => 0,
     # @ple The directory to put generated datasets in
     scratchDir              => undef,
     # @ple Suppress clean up of the test machines if one of these named error
     #      types occurs.
     suppressCleanupOnError  => ["Verify"],
     # @ple Whether to use a filesystem. Should only be set by tests.
     useFilesystem           => 0,
     # @ple The directory to put the user tool binaries in
     userBinaryDir           => undef,
     # @ple Use the dmlinux src rpm for testing.
     useDmLinuxModule        => 0,
     # @ple Use the installed kernel module, dm-vdo
     useUpstreamModule       => 0,
    );

my @SRPM_NAMES
  = (
     "src/srpms/kmod-kvdo-$VDO_VERSION-*.src.rpm",
     "src/srpms/vdo-$VDO_VERSION-*.src.rpm",
    );

my @RPM_NAMES
  = (
     "archive/kmod-kvdo-$VDO_VERSION-1.*.rpm",
     "archive/vdo-$VDO_VERSION-1.*.rpm",
     "archive/vdo-support-$VDO_VERSION-1.*.rpm",
    );

my @UPSTREAM_NAMES
  = (
     "src/packaging/dmlinux/build/SRPMS/kmod-kvdo-$VDO_VERSION-*.src.rpm",
     "src/srpms/vdo-$VDO_VERSION-*.src.rpm",
    );

my @SHARED_FILES
  = (
     "src/c++/third/fio/fio",
     "src/c++/tools/fsync",
     "src/c++/vdo/bin/checkerboardWrite",
     "src/c++/vdo/bin/corruptPBNRef",
     "src/c++/vdo/bin/corruptpbnref",
     "src/c++/vdo/bin/udsCalculateSize",
     "src/c++/vdo/bin/vdoFillIndex",
     "src/c++/vdo/bin/vdoCalculateSize",
    );

# Used to set the raidType property for RAID devices when calling
# createTestDevice.
my %RAID_TYPES
  = (
     raid   => 0,
     raid0  => 0,
     raid1  => 1,
     raid5  => 5,
     raid10 => 10,
    );

########################################################################
# @inherit
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self = $class->SUPER::new(@_);

  assertEqualNumeric($self->{blockSize}, $DEFAULT_BLOCK_SIZE);

  return $self;
}

########################################################################
# Select the default block size for any filesystem in the test.
#
# @return the block size for the filesystem to use.
##
sub _getDefaultFSBlockSize {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->{emulate512Enabled}) {
    return $self->{blockSize};
  }

  # VDO-3443: XFS has a bug with a 1k block size that we provoke.
  if (defined($self->{fsType}) && ($self->{fsType} =~ m/xfs/)) {
    return 2048;
  }

  # 1024 is the lowest value supported by all of ext3, ext4, and xfs.
  return 1024;
}

########################################################################
# Returns the base amount of memory a host needs to run its system software,
# let us ssh into it, etc.  Any memory needed for test programs needs to be
# added in on top of this value.
#
# @param host    The target host
#
# @return The number of bytes of memory needed
##
sub _minimumMemory {
  my ($host) = assertNumArgs(1, @_);
  # This is supposed to represent the amount of memory needed to run the
  # base system, and install VDO, but not run it. We have a separate
  # calculation for the amount of memory the VDO will use,
  # applicationMemoryNeeded(), and we assume if we can install VDO via
  # dkms (which uses around 130M) we can run fio, genDataBlocks, etc.
  #
  # Fedora 29 measured 2019-08-02 needed 200M for pre-VDO operation.
  # Fedora 30 measured 2020-01-27 needed 200M for pre-VDO operation.
  # Fedora 31 measured 2020-03-13 needed 200M for pre-VDO operation.
  # Fedora 32 not measured; assume it needs 200M for pre-VDO operation.
  # Fedora 33 not measured; assume it needs 200M for pre-VDO operation.
  # Fedora 34 not measured; assume it needs 200M for pre-VDO operation.
  # Fedora 35 not measured; assume it needs 200M for pre-VDO operation.
  # Fedora 36 not measured; assume it needs 200M for pre-VDO operation.
  # Fedora 37 measured 2023-07-24 needed 400M for pre-VDO operation.
  # Fedora 38 measured 2023-07-24 needed 400M for pre-VDO operation.
  # Fedora 39 not measured; assume it needs 400M for pre-VDO operation.
  # Fedora 40 not measured; assume it needs 400M for pre-VDO operation.
  # Fedora 41 not measured; assume it needs 400M for pre-VDO operation.
  # Fedora 42 not measured; assume it needs 400M for pre-VDO operation.
  # Fedora 43 not measured; assume it needs 400M for pre-VDO operation.
  # FEDORANEXT not measured; assume it needs 400M for pre-VDO operation.
  # RHEL8 measured 2023-07-24 needed 400M for pre-VDO operation.
  # RHEL9 measured 2023-07-24 needed 400M for pre-VDO operation. (VDO-5559)
  # RHEL10 not measured; assume it needs 400M for pre-VDO operation. (VDO-5899)
  my %DISTRO_MEMORY_REQUIREMENTS
    = (
       FEDORA39   => 400 * $MB,
       FEDORA40   => 400 * $MB,
       FEDORA41   => 400 * $MB,
       FEDORA42   => 400 * $MB,
       FEDORA43   => 400 * $MB,
       FEDORANEXT => 400 * $MB,
       RAWHIDE    => 400 * $MB,
       RHEL8      => 400 * $MB,
       RHEL9      => 400 * $MB,
       RHEL10     => 400 * $MB,
      );
  my $distro   =  getDistroInfo($host);
  assertDefined($DISTRO_MEMORY_REQUIREMENTS{$distro});
  return $DISTRO_MEMORY_REQUIREMENTS{$distro};
}

########################################################################
# Returns (an estimate of) the memory needed for applications and test programs
# to be run during the test.
#
# @return   The number of bytes needed
##
sub applicationMemoryNeeded {
  my ($self) = assertNumArgs(1, @_);
  # This just accounts for the Albireo index and VDO.  We fudge everything else
  # with the base memory requirements.
  return ($self->{memorySize} || 1) * $GB + $VDO_REQUIRED_MEMORY;
}

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->{prereservedHosts} =
    $self->canonicalizeHostnames($self->{clientNames});

  if (defined($self->{fsType}) && ($self->{fsType} =~ m/^nfs-/)) {
    $self->{numNfsclients} ||= 1;
    $self->{nfsclientClass} //= $self->{clientClass};
  };
  $self->{fsBlockSize} //= $self->_getDefaultFSBlockSize();
  $self->reserveHosts();

  $self->SUPER::set_up();

  $self->{scratchDir} = makeFullPath($self->{workDir}, 'scratch');
  $self->{userBinaryDir} = makeFullPath($self->{runDir}, 'executables');

  foreach my $byteSize (qw(blockMapCacheSize
                           logicalSize
                           loopSize
                           lvmSize
                           physicalSize)) {
    if (defined($self->{$byteSize})) {
      $self->{$byteSize} = parseBytes($self->{$byteSize});
    }
  }

  if ($self->{lowMemoryTest}) {
    my @limits = map { _minimumMemory($_) } @{$self->{clientNames}};
    my $minimumMemory = max(@limits);
    # That's for the basic distribution plus ssh into it etc.
    # For Albireo and maybe VDO:
    $minimumMemory += $self->applicationMemoryNeeded();
    $log->info("computed kernel memory limit: " . sizeToText($minimumMemory));
    setupKernelMemoryLimiting($self->{clientNames}, $minimumMemory, 0);
  }

  if ($self->{rawhideKernel}) {
    setupRawhideKernel($self->{clientNames});
  }

  # Make the device stack if specified
  if (defined($self->{blockTrace})) {
    $self->{_traceTypes} = { map({ ($_, 1) } @{$self->{blockTrace}}) };
  }

  # If devicetype is not specifically set, decide what kind of default bottom
  # device to make.
  if (!defined($self->{deviceType}) || $self->{deviceType} eq '') {
    my $stack = $self->getStorageStack();
    my $disks = $stack->getUserMachine()->selectDefaultRawDevices();
    if (scalar(@{$disks}) > 1) {
      $self->{deviceType} = "raid";
    } else {
      $self->{deviceType} = "raw";
    }
  }

  # if deviceType is none, don't create any devices
  if ($self->{deviceType} ne 'none') {
    foreach my $type (reverse(split('-', $self->{deviceType}))) {
      $self->createTestDevice($type);
    }
  }

  foreach my $host (@{$self->{nfsclientNames}}) {
    assertCommand($host, "mkdir -p $self->{runDir}");
  }

  if ($self->{useFilesystem}) {
    $self->createFileSystem($self->getDevice());
  }
}

########################################################################
# Reserve the hosts needed for the test.
#
# May be overridden by subclasses.
##
sub reserveHosts {
  my ($self) = assertNumArgs(1, @_);
  $self->reserveHostGroups("client", "nfsclient");
}

########################################################################
# Create a test device
#
# @param deviceType  Type of device to create
# @oparam extra      Extra arguments to be passed to the new() method.
#
# @return the created Permabit::BlockDevice
##
sub createTestDevice {
  my ($self, $deviceType, %extra) = assertMinArgs(2, @_);
  # clone $extra so as not to modify it.
  if ($self->{_traceTypes}{$deviceType}) {
    $extra{tracing} //= 1;
  }
  if (defined($RAID_TYPES{$deviceType})) {
    $extra{raidType} //= $RAID_TYPES{$deviceType};
    $deviceType = "raid";
  }
  return $self->getStorageStack()->create($deviceType, { %extra });
}

########################################################################
# Destroy a test device.
#
# @param device  The device to destroy
##
sub destroyTestDevice {
  my ($self, $device) = assertNumArgs(2, @_);
  $self->getStorageStack()->destroy($device);
}

########################################################################
# Get the storage stack, constructing it if necessary.
#
# @return The storge stack
##
sub getStorageStack {
  my ($self) = assertNumArgs(1, @_);
  if (!defined($self->{_storageStack})) {
    $self->{_storageStack} = Permabit::StorageStack->new($self);
    $self->{_storageStack}->setDeviceCreateHook(sub {
                                                  $self->deviceCreated(@_);
                                                });
  }
  return $self->{_storageStack};
}

########################################################################
# Notify a test that a device has been created.
#
# @param device  The device which was just created
##
sub deviceCreated {
  my ($self, $device) = assertNumArgs(2, @_);

  # After creating a device, do any associated manual wait points
  foreach my $subclass (qw(TestDevice::Managed::Corruptor
                           Crypt
                           TestDevice::Dory
                           TestDevice::Fua
                           Loop
                           TestDevice::Managed::Tracer
                           VDO)) {
    if (!$device->isa("Permabit::BlockDevice::$subclass")) {
      next;
    }

    my $host = $device->getMachine()->getName();
    my $path = $device->getSymbolicPath();
    my $name = $subclass;
    $name =~ s/^.*:://;
    $self->manualWaitPoint("${name}DeviceCreated",
                           "$subclass set up on $host device $path");
  }

  return $device;
}

########################################################################
# Create a slice of the VDO device.  Arguments are passed in name-value
# pairs. Block and filesystem options should not be mixed.
#
# @oparam blockCount  Block count  (defaults to whole device)
# @oparam blockSize   Block size   (defaults to VDO blocksize or 4K)
# @oparam offset      Block offset (defaults to 0)
# @oparam fs          Filesystem   (if not defined, use device directly)
# @oparam fileCount   File count   (defaults to 1)
# @oparam totalBytes  Total bytes for files (defaults to whole filesystem)
#
# @return a Permabit::GenSlice
##
sub createSlice {
  my ($self, %args) = assertMinArgs(1, @_);
  return Permabit::GenSlice->new(device => $self->getDevice(), %args);
}

########################################################################
# Return the pathname of the expected tarball in the source tree, given
# the module version string, relative to the top of the tree, and thus
# suitable for use in the shared-files list.
#
# @param version  The version string, e.g., "6.3" or "6.3.experimental",
#                 which must also match the directory name the tarball
#                 unpacks into.
#
# @return the relative path of the file
##
sub getTGZNameForVersion {
  my ($self, $version) = assertNumArgs(2, @_);
  return "src/c++/vdo/kernel/$VDO_MODNAME-$version.tgz";
}

########################################################################
# @inherit
##
sub listSharedFiles {
  my ($self) = assertNumArgs(1, @_);
  my @files = ($self->SUPER::listSharedFiles(), @SHARED_FILES);
  if ($self->{useDmLinuxModule}) {
    return (@files, @UPSTREAM_NAMES);
  } elsif ($self->{useDistribution}) {
    return (@files, @RPM_NAMES);
  } else {
    return (@files, @SRPM_NAMES);
  }
}

########################################################################
# @inherit
##
sub shouldSaveLogs {
  my ($self) = assertNumArgs(1, @_);
  return ($self->{blockTrace}
          || $self->{deviceMapperBlockTrace}
          || $self->SUPER::shouldSaveLogs());
}

########################################################################
# @inherit
##
sub getTestHosts {
  my ($self) = assertNumArgs(1, @_);
  return (@{$self->{prereservedHosts}},
          $self->SUPER::getTestHosts(),
          $self->getRSVPer()->getReservedHosts());
}

########################################################################
# Generate a file full of random and optionally compressible data
#
# @param  size     Size of the file (in bytes)
# @oparam name     The name of the file (defaults to '0')
#
# @return  the full pathname of the file
##
sub generateDataFile {
  my ($self, $size, $name) = assertMinMaxArgs(2, 3, @_);
  my $machine = $self->getDevice()->getMachine();
  my $scratchDir = $machine->getScratchDir();

  assertDefined($self->{compressibility});
  assertDefined($self->{compressibleChunkSize});
  my $props = {
               'gen.large.num'               => 1,
               'gen.large.min'               => $size,
               'gen.large.max'               => $size,
               'gen.root.dir'                => $scratchDir,
               'gen.compressible.percentage' => $self->{compressibility},
               'gen.compressible.blockSize'  => $self->{compressibleChunkSize},
              };
  $machine->generateDataSet($props);
  my $path = makeFullPath($scratchDir, "0");
  if (defined($name)) {
    my $requestedPath = makeFullPath($scratchDir, $name);
    $machine->runSystemCmd("mv -f $path $requestedPath");
    $path = $requestedPath;
  }
  return $path;
}

########################################################################
# Get the default host to use for the test
#
# @return The hostname of the default host
##
sub getDefaultHost {
  my ($self) = assertNumArgs(1, @_);
  return $self->{defaultHost} // $self->{clientNames}[0];
}

########################################################################
# Gets a device from the storage stack. By default, returns the top device in
# the stack if the stack is unbranched (branched stacks are not quite supported
# yet). If an optional type is supplied, the upper-most device of that type is
# returned.
#
# @oparam The type of device desired
#
# @return the Permabit::BlockDevice
##
sub getDevice {
  my ($self, $type) = assertMinMaxArgs(1, 2, @_);
  if (defined($type)) {
    my @devices = $self->getStorageStack()->getDescendantsOfType($type);
    return pop(@devices);
  }

  return $self->getStorageStack()->getTop();
}

########################################################################
# Gets the last filesystem set up in the test set_up function
#
# @return the Permabit::BlockDevice
##
sub getFileSystem {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_filesystems}[-1];
}

########################################################################
# Gets a UserMachine that is used for running tests. If a machine doesn't
# exist then one will be created and cached.
#
# @oparam name  The hostname.  The default is to assume the the test uses
#               a single client machine.
#
# @return a Permabit::UserMachine for the first clientName host
##
sub getUserMachine {
  my ($self, $name) = assertMinMaxArgs([undef], 1, 2, @_);
  $name ||= $self->getDefaultHost();
  assertDefined($name);
  if (!defined($self->{_machines}{$name})) {
    my %params = (
                  hostname         => $name,
                  hungTaskWarnings => $self->{hungTaskWarnings},
                  nfsShareDir      => $self->{nfsShareDir},
                  scratchDir       => $self->{scratchDir},
                  workDir          => $self->{workDir},
                  userBinaryDir    => $self->{userBinaryDir},
                 );
    $self->{_machines}{$name} = Permabit::UserMachine->new(%params);
  }

  return $self->{_machines}{$name};
}

########################################################################
# Restart a device's host machine, and wait for it to be ready again.
#
# @param device  The device whose machine should be restarted
##
sub rebootMachineForDevice {
  my ($self, $device) = assertNumArgs(2, @_);
  assertDefined($device);
  $self->getStorageStack()->stopAll();

  $device->getMachine()->restart();

  $self->getStorageStack()->startAll();
  $device->waitForDeviceReady();
}

########################################################################
# Crash a device's host machine, and wait for it to be ready again.
#
# @param device  The device whose machine should be restarted
##
sub emergencyRebootMachineForDevice {
  my ($self, $device) = assertNumArgs(2, @_);
  assertDefined($device);

  my $machine = $device->getMachine();
  $machine->emergencyRestart();

  # We always expect a rebuild after a sudden machine reboot.
  $self->getStorageStack()->recoverAll();
  $device->waitForDeviceReady();
}

########################################################################
# Create a volume group
#
# @oparam storageDevice  The storage device
# @oparam name           The name for the new volume group
#
# @return a volume group
##
sub createVolumeGroup {
  my ($self, $storageDevice, $name) = assertMinMaxArgs(1, 3, @_);
  return $self->getStorageStack()->createVolumeGroup($storageDevice, $name);
}

########################################################################
# Get a Dory device that exists in the device hierarchy.
##
sub getDoryDevice {
  my ($self) = assertNumArgs(1, @_);
  return $self->getDevice('Permabit::BlockDevice::TestDevice::Dory');
}

########################################################################
# Get a VDO device that exists in the device hierarchy.
##
sub getVDODevice {
  my ($self) = assertNumArgs(1, @_);
  return $self->getDevice('Permabit::BlockDevice::VDO');
}

########################################################################
# Wait for the dedupe operations to finish, and then read the current VDO
# statistics.  If there is no VDO running, this is a no-op.
#
# @return the VDO statistics or undef if there is no running VDO
##
sub getVDOStats {
  my ($self) = assertNumArgs(1, @_);
  return $self->getStorageStack()->getVDOStats();
}

########################################################################
# Get and log the VDO statistics, then assert that each expected value matches
# the statistic value obtained from VDO.
#
# @param expected  The hashref of statistic keys and values to check
##
sub assertVDOStats {
  my ($self, $expected) = assertNumArgs(2, @_);
  my $device = $self->getVDODevice();
  my $stats = $self->getVDOStats();
  $stats->logStats($device->getDevicePath());

  foreach my $key (sort(keys(%$expected))) {
    assertEqualNumeric($expected->{$key}, $stats->{$key}, "'$key' statistic");
  }
}

########################################################################
# Create and mount a filesystem on the specified device
#
# @param  device    the device to create the filesystem on
#
# @return a filesystem
##
sub createFileSystem {
  my ($self, $device) = assertNumArgs(2, @_);
  my %FS_TYPE_CLASS = (
                       "ext3" => "Permabit::FileSystem::Ext3",
                       "ext4" => "Permabit::FileSystem::Ext4",
                       "xfs"  => "Permabit::FileSystem::Xfs",
                      );
  assertDefined($self->{fsType});
  assertDefined($device);
  my $fsType = $self->{fsType};
  my $useNfs = 0;
  if ($fsType =~ m/^nfs-(\w+)$/) {
    $useNfs = 1;
    $fsType = $1;
  }
  my $fsClass = $FS_TYPE_CLASS{$fsType};
  assertDefined($fsClass);
  my $fs = $fsClass->new(device    => $device,
                         blockSize => $self->{fsBlockSize},
                        );
  push(@{$self->{_filesystems}}, $fs);
  $fs->mount();
  $self->manualWaitPoint("FilesystemCreated",
                         "FS set up on " . $device->getMachine()->getName()
                         . " device " . $device->getSymbolicPath()
                         . " mounted on " . $fs->getMountDir());
  if ($useNfs) {
    $fs->exportNfs();
    my $hostname = $self->{nfsclientNames}[0];
    assertDefined($hostname);
    my $machine = $self->getUserMachine($hostname);
    $fs = Permabit::FileSystem::Nfs->new(fs      => $fs,
                                         machine => $machine);
    push(@{$self->{_filesystems}}, $fs);
    $fs->mount();
    $self->manualWaitPoint("FilesystemMount",
                           "NFS set up on " . $machine->getName()
                           . " mounted on " . $fs->getMountDir());
  }
  return $fs;
}

########################################################################
# Save logfiles related to the VDO kernel module generated by this test into
# the given directory.  This method should be overridden by subclasses to save
# their own logfiles related to the kernel module.
#
# @param saveDir  The directory to save the logfiles into
##
sub saveKernelLogFiles {
  my ($self, $saveDir) = assertNumArgs(2, @_);

  $log->info("Saving kernel modules to $saveDir");

  my $stack = $self->getStorageStack();
  if ($stack->isEmpty()) {
    $log->info("No modules saved since no devices are defined");
    return;
  }

  my @devices = reverse($stack->getDescendantsOfType('UNIVERSAL'));
  foreach my $device (@devices) {
    my $host = $device->getMachineName();
    $log->info("Saving kernel modules for client host $host");
    $self->runTearDownStep(sub { $device->saveLogFiles($saveDir); });
  }
}

########################################################################
# @inherit
##
sub run_coda {
  my ($self) = assertNumArgs(1, @_);
  $self->getStorageStack()->check();
  $self->SUPER::run_coda();
}

########################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);

  if (!$self->{suppressCleanup}) {
    # Stop any tasks belonging to this test.  We must do this before trying
    # to teardown any VDO device or filesystem.
    $self->tearDownAsyncTasks();

    # Tear down filesystems.  Do this while stall detection is still active!
    while (my $fs = pop(@{$self->{_filesystems}})) {
      $self->manualWaitPoint("FilesystemUnmount",
                             "Ready to unmount " . $fs->getMountDir()
                             . " filesystem on host "
                             . $fs->getMachine()->getName());
      $self->runTearDownStep(sub { $fs->stop(); });
      if ($self->shouldSaveLogs()) {
        $self->runTearDownStep(sub {$fs->logCopy($self->{runDir})});
      }
    }
  }

  # Save kernel module that was compiled on the test host.
  #
  # This needs to occur during the tear down at the VDOTest inheritance /
  # call-hierarchy level due to the order of calls in overridden functions
  # between superclass and subclass (it's in bottom-up order from subclass
  # to superclass).  Waiting for the saveAllLogFiles() method call inside
  # the SUPER::tear_down() method does not work since the VDO device is
  # destroyed before then.
  if ($self->shouldSaveLogs()) {
    my $saveDir = $self->getDefaultLogFileDirectory();
    saveKernelLogFiles($self, $saveDir);
  }

  if (!$self->{suppressCleanup}) {
    # Tear down devices (includes any VDO device that was created).
    my $stack = $self->getStorageStack();
    $stack->setDeviceDestroyHook(sub { $self->destroyDevice(@_) });
    $stack->setTeardownWrapper(sub { $self->runTearDownStep(@_) });
    $stack->destroyAll();
    delete $self->{_storageStack};

    # Clean up LVM devices files after all devices are destroyed
    $self->runTearDownStep(sub { $self->cleanupLVMDevicesFiles(); });

    # Close the UserMachines because we are about to release our RSVP
    # reservations.
    map { $_->closeForRelease() } values(%{$self->{_machines}});
    delete $self->{_machines};

    if ($self->{lowMemoryTest}) {
      my $removeMemoryLimiting = sub {
        removeKernelMemoryLimiting($self->{clientNames});
      };
      $self->runTearDownStep($removeMemoryLimiting);
    }

    if ($self->{rawhideKernel}) {
      my $removeRawhide = sub {
        removeRawhideKernel($self->{clientNames});
      };
      $self->runTearDownStep($removeRawhide);
    }
  }

  $self->SUPER::tear_down();
}

########################################################################
# Notify the test that a device is to be destroyed. This method is
# set as the device destroy hook of the storage stack during teardown.
##
sub destroyDevice {
  my ($self, $device) = assertNumArgs(2, @_);
  if ($self->{auditVDO}
      && !$self->{useDistribution}
      && $device->isa("Permabit::BlockDevice::VDO")) {
    $self->runTearDownStep(sub { $device->stop() });

    # We want to audit, even if stop failed, because stop might have just
    # failed because we were in read-only mode.
    $self->runTearDownStep(sub {
                             my $result = $device->doVDOAudit();
                             if ($result->{returnValue} != 0) {
                               $self->setFailedTest("vdoAudit failed");
                             }
                           });
  }
}

########################################################################
# Clean up all LVM devices files after all devices are destroyed.
##
sub cleanupLVMDevicesFiles {
  my ($self) = assertNumArgs(1, @_);

  # Get all machines that might have LVM devices files to clean up
  my @machines = values(%{$self->{_machines}});

  foreach my $machine (@machines) {
    $machine->cleanupLVMDevicesFile();
  }
}

########################################################################
# Determine whether the block usage of a VDO device will be predictable if we
# write to the device using the system page cache.
#
# The large body of VDO tests that we developed prior to RHEL8 were only run on
# the X86 architecture, where the page size of the system was 4K which matched
# the block size of VDO and the block size of the page cache.  Some of the test
# expectations assume that the block size of VDO and the block size of the page
# cache will be identical.
#
# When the test writes one 4K block, we commonly assume that this will result
# in one I/O request to VDO, writing one 4K block.  On aarch64 machines, we
# observe that a write of one 4K block results in sixteen I/O requests to VDO,
# each writing one 4K block.  This mismatch means that some of our test
# assertions will fail:
#
#    "dedupe advice valid" assumptions can fail because writing a stream of 4k
#    blocks results in an unpredictable number of writes.  The page cache can
#    write a partial big block followed by a rewrite of the same big block, and
#    during the rewrite we will get dedupe advice valid on the blocks that were
#    rewritten.  In fact, any write of a 4K block can result in a rewrite of
#    its neighboring blocks.
#
#    "logical blocks used" assumptions can fail because writing a 4K block can
#    result in writing zero blocks into its neighbors.
#
# @return true if the we can make valid predictions of the "dedupe advice
#         valid" and "logical blocks used" values.
##
sub canPredictBlockUsage {
  my ($self) = assertNumArgs(1, @_);
  my $pagesize = $self->getDevice()->getMachine()->getPageSize();
  my $predictable = $pagesize eq $PHYSICAL_BLOCK_SIZE;
  $log->info($predictable
             ? "VDO blocks used statistics are predictable"
             : "VDO blocks used statistics are not predictable");
  return $predictable;
}

1;
