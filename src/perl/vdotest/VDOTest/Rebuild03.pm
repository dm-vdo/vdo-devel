##
# Test VDO recovery and the integrity of filesystems atop VDO, making sure
# the filesystem survives intact after a recovery.
#
# $Id$
##
package VDOTest::Rebuild03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEq assertNumArgs);
use Permabit::Constants;
use Permabit::Utils qw(makeFullPath);

use base qw(VDOTest::RebuildBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple An uneven chunk size gives a mixture of compressibilities
     compressibleChunkSize => 6 * $KB,
     # @ple Make each chunk mostly compressible
     compressibility       => 55,
     # @ple Use a fast Dory rebuild, even though Xfs sometimes fails. Never
     #      use a real reboot with a stripfua device; it doesn't survive.
     deviceType            => "lvmvdo-dory",
     # @ple Enable compression by VDO
     enableCompression     => 1,
    );
##

#############################################################################
# Write some data and then do a recovery.  Check that the filesystem can fsck.
##
sub testFsck {
  my ($self) = assertNumArgs(1, @_);
  my $device  = $self->getDevice();
  my $machine = $device->getMachine();
  my $fs      = $self->getFileSystem();

  # Make sure the configuration is written out; this is not a test of
  # vdo configuration resiliency.
  $machine->runSystemCmd("sync");

  my $props = $self->getDataSetProps03($fs->makeSubdir("testFsck"));
  $machine->generateDataSet($props);

  # Do not automatically remount the filesystem when VDO starts.
  $self->recoverAndRestartVDO(0);

  # Verify that the filesystem can be checked
  $fs->fsck();
}

#############################################################################
# Write some data and then do a recovery. Check that the filesystem can mount.
##
sub testMount {
  my ($self) = assertNumArgs(1, @_);
  my $device  = $self->getDevice();
  my $fs      = $self->getFileSystem();
  my $machine = $device->getMachine();

  # Make sure the configuration is written out; this is not a test of
  # vdo configuration resiliency.
  $machine->runSystemCmd("sync");

  my $props = $self->getDataSetProps03($fs->makeSubdir("before"));
  $machine->generateDataSet($props);

  # This will remount the filesystem, which is the point of this test
  $self->recoverAndRestartVDO();

  # Verify that the filesystem can be used
  $props = $self->getDataSetProps03($fs->makeSubdir("after"));
  $machine->generateDataSet($props);
}

#############################################################################
# Touch a new file and take reasonable measures to ensure that the file is
# written to storage and then do a recovery. Check that the filesystem mounts
# cleanly and that the file still exists.
##
sub testTouch {
  my ($self) = assertNumArgs(1, @_);
  my $device  = $self->getDevice();
  my $fs      = $self->getFileSystem();
  my $machine = $device->getMachine();

  # Touch a file, then sync.
  my $subdir = $fs->makeSubdir("testTouch");
  my $touched = makeFullPath($subdir, "touched");
  $machine->runSystemCmd("touch $touched");
  $machine->runSystemCmd("sync");

  $self->recoverAndRestartVDO();

  # Verify that the file is there and contains 0 bytes
  $machine->runSystemCmd("cat $touched | wc -c");
  assertEq($machine->getStdout(), "0\n");
}

#############################################################################
# Touch a new file and take reasonable measures to ensure that its
# directory entry written to storage.  Write some data to the file using
# fsync and then do a recovery.  Check that the filesystem mounts cleanly and
# that the file still exists with expected data.
##
sub testFsync {
  my ($self) = assertNumArgs(1, @_);
  my $device  = $self->getDevice();
  my $fs      = $self->getFileSystem();
  my $machine = $device->getMachine();

  my $dataPath = $self->generateDataFile(1 * $MB, "source");
  $machine->fsync($dataPath);

  # Touch a file.  Sync and sleep for 5 seconds.
  my $subdir = $fs->makeSubdir("testFsync");
  my $file = makeFullPath($subdir, "destination");
  $machine->runSystemCmd("touch $file");
  $machine->runSystemCmd("sync");

  # Write data to the file using fsync
  $machine->dd(
               if   => $dataPath,
               of   => $file,
               conv => "fsync,nocreat"
              );

  $self->recoverAndRestartVDO();

  # Verify that the file was written correctly
  $machine->runSystemCmd("cmp $dataPath $file");
}

#############################################################################
# Make a property hash for genDataSet
#
# @param subdir  Root directory for the data set
#
# @return  the genDataSet properties hashref
##
sub getDataSetProps03 {
  my ($self, $subdir) = assertNumArgs(2, @_);
  my $props = {
               "gen.root.dir"                => $subdir,
               "gen.large.num"               => 100,
               "gen.large.min"               => $MB,
               "gen.large.max"               => $MB,
               "gen.large.numCoalescent"     => 25,
               "gen.compressible.percentage" => $self->{compressibility},
               "gen.compressible.blockSize"  => $self->{compressibleChunkSize},
              };
  return $props;
}

1;
