##
# Tests of 512 byte logical block size reads and writes.
#
# $Id$
##
package VDOTest::Logical512;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use FindBin;
use Permabit::Assertions qw(assertEqualNumeric assertGTNumeric assertNumArgs);
use Permabit::AsyncTask::RunSystemCmd;
use Permabit::Constants qw($SECTOR_SIZE);
use Permabit::Utils qw(makeFullPath);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
    # @ple Type of device set up by VDOTest
    deviceType        => "lvmvdo",
    # @ple Emulate a 512 byte block device
    emulate512Enabled => 1,
    # @ple Enable compression on the VDO device
    enableCompression => 1,
    # @ple Number of write/skip iterations to perform
    iterations        => 1000,
  );
##

#############################################################################
# Create a test data file and a file full of zeroes.
##
sub createSourceFiles {
  my ($self, $writeSectors, $skipSectors) = assertNumArgs(3, @_);

  my $totalSectors   = ($writeSectors + $skipSectors) * $self->{iterations};
  my $fileSize       = $totalSectors * $SECTOR_SIZE;
  $self->{_dataPath} = $self->generateDataFile($fileSize, "testdata");

  my $machine        = $self->getDevice()->getMachine();
  $self->{_zeroPath} = makeFullPath($machine->getScratchDir(), "zeroes");
  $machine->dd(
               bs    => $SECTOR_SIZE,
               count => $totalSectors,
               skip  => 0,
               if    => "/dev/zero",
               of    => $self->{_zeroPath},
              );
}

#############################################################################
# Return an AsyncTask that uses the checkerboardWrite script to write a portion
# of the source file.
#
# @param sourcePath    the path of the data to write
# @param targetPath    the path to write to
# @param writeSectors  the number of sectors to write per write phase
# @param skipSectors   the number of sectors to write per skip phase
# @param skipFirst     if true, start with the skip phase
#
# @return the AsyncTask
##
sub _writePartialFile {
  my ($self, $sourcePath, $targetPath, $writeSectors, $skipSectors, $skipFirst)
    = assertNumArgs(6, @_);
  my $command = join(" ",
                     "sudo",
                     $self->findBinary("checkerboardWrite"),
                     "--source=$sourcePath",
                     "--destination=$targetPath",
                     "--write-sectors=$writeSectors",
                     "--skip-sectors=$skipSectors",
                     "--iterations=$self->{iterations}",
                     ($skipFirst ? ("--skip-first") : ()));
  my $machine = $self->getDevice()->getMachine();
  return Permabit::AsyncTask::RunSystemCmd->new($machine, $command);
}

#############################################################################
# Use two threads to copy data to the VDO as partial writes.
##
sub copyDataToDevice {
  my ($self, $dataPath, $writeSectors, $skipSectors) = assertNumArgs(4, @_);
  my $devPath = $self->getDevice()->getSymbolicPath();
  my $task0 = $self->_writePartialFile($dataPath, $devPath, $writeSectors,
                                       $skipSectors, 0);
  my $task1 = $self->_writePartialFile($dataPath, $devPath, $skipSectors,
                                       $writeSectors, 1);
  $self->getAsyncTasks()->addTasks($task0, $task1);
  $self->getAsyncTasks()->finish();
}

#############################################################################
# Copy data from device to a second file using two threads to read data
# as partial blocks, and verify that the data is what is expected.
##
sub verifyDataFromDevice {
  my ($self, $dataPath, $writeSectors, $skipSectors) = assertNumArgs(4, @_);

  my $device   = $self->getDevice();
  my $machine  = $device->getMachine();
  my $copyPath = makeFullPath($machine->getScratchDir(), "verifyCopy");

  my $task0 = $self->_writePartialFile($device->getSymbolicPath(), $copyPath,
                                       $writeSectors, $skipSectors, 0);
  my $task1 = $self->_writePartialFile($device->getSymbolicPath(), $copyPath,
                                       $skipSectors, $writeSectors, 1);
  $self->getAsyncTasks()->addTasks($task0, $task1);
  $self->getAsyncTasks()->finish();

  $machine->runSystemCmd("cmp --verbose $dataPath $copyPath");
  $machine->runSystemCmd("rm $copyPath");
}

#############################################################################
# Run the basic test pattern for the given write and skip sizes.
##
sub interleavedPartialWrites {
  my ($self, $writeSectors, $skipSectors) = assertNumArgs(3, @_);
  $log->info("Test pattern: Write $writeSectors sectors, "
             . "then skip $skipSectors sectors.");

  $self->createSourceFiles($writeSectors, $skipSectors);

  my $device = $self->getDevice();
  $self->copyDataToDevice($self->{_dataPath}, $writeSectors, $skipSectors);
  $self->verifyDataFromDevice($self->{_dataPath}, $writeSectors, $skipSectors);

  $self->copyDataToDevice($self->{_zeroPath}, $writeSectors, $skipSectors);
  $self->verifyDataFromDevice($self->{_zeroPath}, $writeSectors, $skipSectors);

  my $stats = $self->getVDOStats();
  assertEqualNumeric(0, $stats->{"data blocks used"});
  my $machine = $device->getMachine();
  $machine->runSystemCmd("rm $self->{_dataPath} $self->{_zeroPath}");
}

#############################################################################
# Check partial write stats to make sure they changed.
##
sub checkPartialWriteStats {
  my ($self, $initialStats, $finalStats) = assertNumArgs(3, @_);
  assertGTNumeric($finalStats->{"bios in partial write"},
                  $initialStats->{"bios in partial write"},
                  "Partial writes occur");
  assertEqualNumeric($finalStats->{"bios in partial write"},
                     $finalStats->{"bios acknowledged partial write"},
                     "Bios get acknowledged");
}

#############################################################################
# Test a variety of interesting patterns of data with unaligned writes.
##
sub testUnalignedWrites {
  my ($self) = assertNumArgs(1, @_);
  foreach my $writeSectorCount (2, 5, 6) {
    foreach my $skipSectorCount (4, 7) {
      my $initialStats = $self->getVDOStats();
      $self->interleavedPartialWrites($writeSectorCount, $skipSectorCount);
      $self->checkPartialWriteStats($initialStats, $self->getVDOStats());
    }
  }
}

#############################################################################
# Test writing aligned 4K data. By writing sectors only in groups of 8, this
# test writes 4K-aligned data, which should not produce partial writes in VDO.
##
sub testAlignedWrites {
  my ($self) = assertNumArgs(1, @_);
  my $initialStats = $self->getVDOStats();
  $self->interleavedPartialWrites(8, 8);
  my $finalStats = $self->getVDOStats();
  assertEqualNumeric($finalStats->{"bios in partial write"},
                     $initialStats->{"bios in partial write"},
                     "Partial writes do not occur");
}

1;
