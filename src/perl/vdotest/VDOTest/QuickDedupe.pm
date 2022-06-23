##
# VDO deduplication test on a filesystem that writes multiple copies
# of the same data in a short span of time.
#
# $Id$
##
package VDOTest::QuickDedupe;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNear assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The number of files to write that are identical to other files.
     coalescentFiles => 16,
     # @ple Use a VDO device
     deviceType      => "lvmvdo",
     # @ple The size of each file.
     fileSize        => 4 * $MB,
     # @ple The total number of files to write.
     totalFiles      => 32,
     # @ple This test requires a filesystem
     useFilesystem   => 1,
    );
##

#############################################################################
# @inherit
##
sub getDataSet {
  my ($self, $mountPoint) = assertNumArgs(2, @_);

  my $props = {
               "gen.large.num"           => $self->{totalFiles},
               "gen.large.min"           => $self->{fileSize},
               "gen.large.max"           => $self->{fileSize},
               "gen.large.numCoalescent" => $self->{coalescentFiles},
               "gen.root.dir"            => $mountPoint,
              };
  return $props;
}

#############################################################################
# Write several identical files in a short span on time, and check for
# deduplication.
##
sub testMultipleInFlight {
  my ($self) = assertNumArgs(1, @_);

  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $fs = $self->getFileSystem();
  my $mountPoint = $fs->getMountDir();
  $machine->fsync($mountPoint);

  my $initialStats = $device->getVDOStats();

  ## This is 2000 blocks per file.
  $machine->generateDataSet($self->getDataSet($mountPoint));
  $machine->fsync($mountPoint);

  # Check some stuff
  my $vdoStats = $device->getVDOStats() - $initialStats;
  $vdoStats->logStats("VDO Stats for " . $device->getSymbolicPath());
  my $uniqueFiles = $self->{totalFiles} - $self->{coalescentFiles};
  my $expectedDataUseRatio = $uniqueFiles / $self->{totalFiles};
  my $actualDataUseRatio
    = $vdoStats->{"data blocks used"} / $vdoStats->{"logical blocks used"};
  assertNear($expectedDataUseRatio, $actualDataUseRatio, 0.01,
             "deduplication percentage");
}

1;
