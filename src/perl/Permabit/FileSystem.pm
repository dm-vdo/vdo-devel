##
# Perl base class for linux filesystem objects
#
# $Id$
##
package Permabit::FileSystem;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertMinMaxArgs
  assertNe
  assertNumArgs
  assertRegexpMatches
  assertTrue
  assertType
);
use Permabit::Constants;
use Permabit::Utils qw(makeFullPath retryUntilTimeout);
use Storable qw(dclone);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

# TODO: Add a method called remount() to remount an already mounted
#       filesystem after the mountOptions have been changed.

#############################################################################
# @paramList{new}
my %PROPERTIES
  = (
     # @ple the block size for the filesystem
     blockSize    => undef,
     # @ple the BlockDevice, which must be defined by the caller for local
     #      filesystems
     device       => undef,
     # @ple whether we're exported via NFS or not
     exported     => 0,
     # @ple the type of linux fileSystem (ext3, ext4, etc), which must be
     #      defined by the subclass.  This is used with a -t option to many
     #      linux filesystem commands.
     fsType       => undef,
     # @ple the UserMachine, which is gotten from the BlockDevice for local
     #      filesystems, or is defined by the caller for remote filesystems.
     machine      => undef,
     # @ple the mkfs options for the filesystem (-m 0, -q, ...)
     mkfsOptions  => [],
     # @ple pathname of the mount directory
     mountDir     => $DEFAULT_KVDO_DIR,
     # @ple the mount options for the filesystem (nobarrier, discard, ...)
     mountOptions => [],
     # @ple whether we're mounted or not
     mounted      => 0,
    );
##

#############################################################################
# Creates a C<Permabit::FileSystem>.  Should be invoked from a subclass.
#
# @params{new}
#
# @return a new C<Permabit::FileSystem>
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  # We are only used as a base class.
  assertNe(__PACKAGE__, $class);
  my $self = bless {%{ dclone(\%PROPERTIES) },
                    # Overrides previous values
                    @_ }, $class;

  # For local filesystems the caller must supply a device.  For remote
  # filesystems the caller must supply a machine.
  if ($self->isLocal()) {
    assertDefined($self->{device});
    assertType("Permabit::BlockDevice", $self->{device});
    $self->{machine} = $self->{device}->getMachine();
  }
  assertDefined($self->{fsType});
  assertDefined($self->{machine});
  assertType("Permabit::UserMachine", $self->{machine});

  if ($self->isLocal()) {
    $self->mkfs();
  }
  return $self;
}

#############################################################################
# Dump important parts of a filesystem to a directory. Does nothing for most
# filesystems.
#
# @param toDir  The directory to which to dump, if any.
##
sub logCopy {
  my ($self, $toDir) = assertNumArgs(2, @_);
}

#############################################################################
# Export the filesystem
##
sub exportNfs {
  my ($self) = assertNumArgs(1, @_);
  assertTrue($self->isLocal());
  assertTrue($self->{mounted});
  if (!$self->{exported}) {
    my $machine = $self->{machine};
    # Start the nfs service. Systems with systemd use systemctl; others
    # use service
    my $command
      = "sudo touch /etc/exports"
        . " && if which systemctl;"
             . " then sudo systemctl start nfs-server.service;"
           . " elif test -f /etc/init.d/nfs-kernel-server;"
             . " then sudo service nfs-kernel-server start;"
             . " else sudo service nfs start;"
            . "fi";
    $machine->runSystemCmd("$command");
    my $options = "async,rw,no_root_squash";
    $machine->runSystemCmd("sudo exportfs -i -o $options :$self->{mountDir}");
    $self->{exported} = 1;
  }
}

#############################################################################
# Fsck our filesystem
##
sub fsck {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->{machine};
  my $devPath = $self->{device}->getSymbolicPath();
  $machine->runSystemCmd("sudo fsck -t $self->{fsType} -a $devPath");
}

#############################################################################
# Get the number of free blocks in the filesystem
#
# @param blockSize  The size of a block
#
# @return the number of free blocks
##
sub getFreeBlocks {
  my ($self, $blockSize) = assertNumArgs(2, @_);
  my $machine = $self->{machine};
  $machine->runSystemCmd("df -B $blockSize $self->{mountDir}");
  my $dfOutput = $machine->getStdout();
  assertRegexpMatches(qr/\s+(\d+)\s+\d+%\s+\Q$self->{mountDir}\E/, $dfOutput);
  # Extract the number of free blocks from the df standard output.
  # (Hint: do a "df -B 4096" to see what the df output looks like.)
  $dfOutput =~ m/\s+(\d+)\s+\d+%\s+\Q$self->{mountDir}\E/;
  return $1;
}

#############################################################################
# Get the name of the host the filesystem is on
#
# @return the hostname
##
sub getHostname {
  my ($self) = assertNumArgs(1, @_);
  return $self->{machine}->getName();
}

#############################################################################
# Get the UserMachine where the filesystem is mounted
#
# @return the UserMachine
##
sub getMachine {
  my ($self) = assertNumArgs(1, @_);
  return $self->{machine};
}

#############################################################################
# Get the block size option, to use with mkfs to make our filesystem.
#
# @return the mkfs option to enable the specified block size
##
sub getBlockSizeMkfsOption {
  my ($self) = assertNumArgs(1, @_);
  # Most filesystems use the -b option (and complain when it doesn't match
  # the device's optimal block size: VDO-2448).
  return "-b $self->{blockSize} -F";
}

#############################################################################
# Get the options to use with mkfs to make our filesystem
#
# @return the mkfs options
##
sub getMkfsOptions {
  my ($self) = assertNumArgs(1, @_);
  my @options = @{$self->{mkfsOptions}};
  if (defined($self->{blockSize})) {
    push(@options, $self->getBlockSizeMkfsOption());
  }
  return @options;
}

#############################################################################
# Get the mount directory
#
# @return the mount directory
##
sub getMountDir {
  my ($self) = assertNumArgs(1, @_);
  return $self->{mountDir};
}

#############################################################################
# Takes any mount options required by the block device and combines them with
# the filesystem specific options to generate a full list of options to pass
# to mount(8)
#
# @oparam readonly  Whether to mount read only
#
# @return A string of mount options or an empty string if there are no options
##
sub getMountOptions {
  my ($self, $readonly) = assertMinMaxArgs([0], 1, 2, @_);
  my @opts;
  if ($self->isLocal()) {
    push(@opts, @{$self->{device}->getMountOptions()});
  }
  push(@opts, @{$self->{mountOptions}});
  if ($readonly) {
    push(@opts, "ro,noload");
  }
  if (@opts) {
    return "-o " . join(",", @opts);
  }
  return "";
}

#############################################################################
# Get the file system to be mounted.  The mount(8) man page calls this
# command line argument "special".  It is either a block device for a local
# filesystem or a string naming a remote filesystem.
#
# @return A string naming the filesystem to be mounted
##
sub getMountSpecial {
  my ($self) = assertNumArgs(1, @_);
  assertTrue($self->isLocal());
  return $self->{device}->getSymbolicPath();
}

#############################################################################
# Is this a local or remote filesystem?  Remote filesystems will override
# this method.
#
# @return true if this is a local filesystem
##
sub isLocal {
  my ($self) = assertNumArgs(1, @_);
  return 1;
}

#############################################################################
# Make a subdirectory of the mountpoint, and return its path name
#
# @param name   The name of the subdirectory
#
# @return the path name
##
sub makeSubdir {
  my ($self, $name) = assertNumArgs(2, @_);
  my $path = makeFullPath($self->getMountDir(), $name);
  $self->{machine}->runSystemCmd("mkdir -p $path");
  return $path;
}

#############################################################################
# Make our filesystem
##
sub mkfs {
  my ($self) = assertNumArgs(1, @_);
  assertTrue($self->isLocal());
  my $device = $self->{device};
  my $machine = $self->{machine};
  my $devPath = $device->getSymbolicPath();
  my $opts = join(" ", $self->getMkfsOptions());
  $machine->runSystemCmd("sudo mkfs -t $self->{fsType} $opts $devPath");
  sleep(1);
  $machine->runSystemCmd("sudo chmod 666 " . $device->getDevicePath());
}

#############################################################################
# Mount the filesystem
#
# @oparam readonly  Whether to mount read-only
##
sub mount {
  my ($self, $readonly) = assertMinMaxArgs([0], 1, 2, @_);

  my $machine = $self->{machine};
  my $options = $self->getMountOptions($readonly);
  my $special = $self->getMountSpecial();
  my $cmd = "sudo mkdir -p $self->{mountDir}"
            . " && sudo mount -t $self->{fsType}"
            . " $options $special $self->{mountDir}";

  $machine->runSystemCmd($cmd);
  $self->{mounted} = 1;
  if ($self->isLocal() && !$readonly) {
    $machine->runSystemCmd("sudo chmod 777 $self->{mountDir}");
  }
}

#############################################################################
# Resize our filesystem.
##
sub resizefs {
  my ($self) = assertNumArgs(1, @_);
  confess("This filesystem type does not support resizefs");
}

#############################################################################
# Stop the filesystem
##
sub stop {
  my ($self) = assertNumArgs(1, @_);
  $self->unexport();
  $self->unmount();
}

#############################################################################
# Unexport the filesystem
##
sub unexport {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{mounted} && $self->{exported}) {
    $self->{machine}->runSystemCmd("sudo exportfs -u *:$self->{mountDir}");
  }
  $self->{exported} = 0;
}

#############################################################################
# Unmount the filesystem
##
sub unmount {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->{machine};
  $self->unexport();
  if ($self->{mounted}) {
    # Commands to unmount should have a timeout associated, lest a filesystem
    # bug cause a hang until the test is shot. In particular, VDO-4132 records
    # such a bug on XFS.
    $machine->runSystemCmd("timeout -s KILL 1200 sync; timeout -s KILL 1200 sync");
    my $umountSub = sub {
      $machine->sendCommand("sudo timeout -s KILL 1200 umount $self->{mountDir}");
      my $status = $machine->sendCommand("mount | grep $self->{mountDir}");
      return ($status != 0);
    };
    retryUntilTimeout($umountSub, "umount of $self->{mountDir} timed out",
                      2 * $MINUTE, 5);
  }
  $machine->runSystemCmd("sudo rm -rf $self->{mountDir}");
  $self->{mounted} = 0;
}

#############################################################################
# Query whether the mounted filesystem issues discard requests
#
# @return true if the filesystem issues discard requests
##
sub usingDiscard {
  my ($self) = assertNumArgs(1, @_);
  return scalar(grep { $_ eq "discard" } @{$self->{mountOptions}});
}

1;
