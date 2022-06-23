##
# Basic VDO deduplication test on a filesystem.
#
# $Id$
##
package VDOTest::BasicFSDedupe;

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
our %PROPERTIES = (
  # @ple Use a VDO device
  deviceType    => "lvmvdo",
  # @ple This test requires a filesystem
  useFilesystem => 1,
);
##

#############################################################################
# @inherit
##
sub getDataSet {
  my ($self, $dataPath) = assertNumArgs(2, @_);

  my $props = {
               "gen.large.num" => 32,
               "gen.large.min" => 8 * $MB,
               "gen.large.max" => 8 * $MB,
               "gen.root.dir"  => $dataPath,
              };
  return $props;
}

#############################################################################
# Write some blocks, read them back and verify the data have not changed.
# Then write the blocks again, expecting complete dedupe.
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);

  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $fs = $self->getFileSystem();
  my $baseDir = $fs->makeSubdir("original");
  my $copyDir = $fs->makeSubdir("copy1");
  $machine->fsync($copyDir);

  my $initialStats = $device->getVDOStats();

  my $dataPath = $self->{scratchDir};
  $machine->generateDataSet($self->getDataSet($dataPath));

  # Copy the dataset onto the deduplicating device.
  $machine->runSystemCmd("cp -r $dataPath $baseDir");
  $machine->fsync($baseDir);

  my $vdoStats = $device->getVDOStats() - $initialStats;
  $vdoStats->logStats("First write: VDO Stats for "
                      . $device->getSymbolicPath());
  my $expectedDataUseRatio = 1.0;
  my $actualDataUseRatio
    = $vdoStats->{"data blocks used"} / $vdoStats->{"logical blocks used"};
  assertNear($expectedDataUseRatio, $actualDataUseRatio, 0.01,
             "deduplication percentage");

  # Copy the dataset.
  $machine->runSystemCmd("cp -r $dataPath $copyDir");
  $machine->fsync($copyDir);

  $vdoStats = $device->getVDOStats() - $initialStats;
  $vdoStats->logStats("Copy: VDO Stats for " . $device->getSymbolicPath());
  $expectedDataUseRatio = 1 / 2;
  $actualDataUseRatio
    = $vdoStats->{"data blocks used"} / $vdoStats->{"logical blocks used"};
  assertNear($expectedDataUseRatio, $actualDataUseRatio, 0.01,
             "deduplication percentage");
}

1;
