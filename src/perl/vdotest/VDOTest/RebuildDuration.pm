##
# Test VDO recovery duration
#
# Do a recovery after writing a data pattern touches many logical block map
# pages and fills the journal. Determine the amount of time that is required
# for offline recovery.
#
# Verify that IO can be performed during online recovery, and determine how
# long online recovery takes.
#
# $Id$
##
package VDOTest::RebuildDuration;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEq
  assertEvalErrorMatches
  assertLTNumeric
  assertNumArgs
  assertNENumeric
  assertTrue
);
use Permabit::CommandString::FIO;
use Permabit::Constants;
use Permabit::Utils qw(sizeToLvmText);

use base qw(VDOTest::RebuildBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple don't audit on failures... the output is 8GB
     auditVDO          => 0,
     # @ple make sure the page cache is large enough to hold enough dirty pages
     blockMapCacheSize => 4 * $GB,
     # @ple class of machine, requires large volume
     clientClass       => "VDO-PMI",
     # @ple the type of vdo device to use
     deviceType        => "lvmvdo",
     # @ple volume size
     logicalSize       => 256 * $TB,
     # @ple physical size of underlying storage
     physicalSize      => 767 * $GB,
     # @ple use the minimum slab size to keep slab journals from flushing
     slabBits          => $SLAB_BITS_TINY,
     # @ple whether to use a file system
     useFilesystem     => 0,
    );
##

my %DEFAULT_FIO_OPTIONS
  = (
     directIo  => 1,     # use hardware raid by default
     ioDepth   => 128,
     ioEngine  => "libaio",
     ioType    => "write",
     jobs      => 1,
     rwmixread => undef, # don't care about read mix
  );

########################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();
  # Don't wait for the index to rebuild.
  $self->getDevice()->{expectIndexer} = 0;
}

########################################################################
# Determine the amount of logical space in each block map page.
##
sub _getBlockMapEntriesPerPage {
  my ($self) = assertNumArgs(1, @_);

  # XXX It would be nice if this could be read from the kernel module symbols
  # instead of being redundantly hard-coded here.
  my $overheadBytes = 36;
  my $bytesPerEntry = 5;
  return int(($self->{blockSize} - $overheadBytes) / $bytesPerEntry);
}

########################################################################
# Write a data pattern that touches logical pages while filling up the recovery
# journal.
##
sub testRebuildDuration {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  my $journalThreshold = (32 * 1024) - 1;
  my $entriesPerBlock  = 311;
  my $entriesNeeded    = $journalThreshold * $entriesPerBlock;
  # There will be 3 entries per data block write:
  # block map allocation, data increment, and data decrement.
  my $dataBlocks = int($entriesNeeded / 3);

  # Determine the amount of logical space in each block map page.
  my $stride = $self->{blockSize} * $self->_getBlockMapEntriesPerPage();

  $log->info("Issuing $dataBlocks writes.");
  my $fioOptions
    = {
       %DEFAULT_FIO_OPTIONS,
       blockSize      => $self->{blockSize},
       filename       => $device->getSymbolicPath(),
       ioTypeModifier => $stride - $self->{blockSize},
       writePerJob    => $dataBlocks * $stride,
      };
  my $fio = Permabit::CommandString::FIO->new($self, $fioOptions);
  $device->getMachine()->assertExecuteCommand("($fio)");

  $self->getVDOStats()->logStats($device->getDevicePath());
  $self->recoverAndRestartVDO();
}

########################################################################
# Fill the physical storage of the VDO.
##
sub _fillVDO {
  my ($self)  = assertNumArgs(1, @_);
  my $device  = $self->getDevice();
  my $machine = $device->getMachine();

  my $vdoStats    = $self->getVDOStats();
  my $totalBlocks = $self->{physicalSize} / $self->{blockSize};
  my $dataBlocks  = $totalBlocks - $vdoStats->{"overhead blocks used"};

  # Determine the amount of logical space in each block map page.
  my $stride = $self->{blockSize} * $self->_getBlockMapEntriesPerPage();

  $log->info("Zeroing one block in each $stride bytes over $dataBlocks blocks"
             . " to ensure the block map tree is allocated");
  my $fioOptions
    = {
       %DEFAULT_FIO_OPTIONS,
       blockSize       => $self->{blockSize},
       filename        => $device->getSymbolicPath(),
       ioTypeModifier  => $stride - $self->{blockSize},
       writePerJob     => $dataBlocks * $self->{blockSize},
       zeroBuffers     => 1,
      };
  my $fio = Permabit::CommandString::FIO->new($self, $fioOptions);
  $machine->assertExecuteCommand("($fio)");

  $vdoStats   = $self->getVDOStats();
  $dataBlocks = $totalBlocks - $vdoStats->{"overhead blocks used"};

  $log->info("Fill the VDO. Issuing $dataBlocks writes.");
  $fioOptions
    = {
       %DEFAULT_FIO_OPTIONS,
       blockSize   => $self->{blockSize},
       filename    => $device->getSymbolicPath(),
       writePerJob => $dataBlocks * $self->{blockSize},
      };
  $fio = Permabit::CommandString::FIO->new($self, $fioOptions);
  $machine->assertExecuteCommand("($fio)");

  return $dataBlocks;
}

########################################################################
# This pattern verifies that the loading of the reference counts for clean
# slabs are deferred.
##
sub testRefCountsDeferredLoad {
  my ($self)  = assertNumArgs(1, @_);
  my $device  = $self->getDevice();
  my $machine = $device->getMachine();

  $self->_fillVDO();

  $self->getVDOStats()->logStats($device->getDevicePath());

  $log->info("Performing clean shutdown and restart VDO");
  $device->restart();

  # Dirty just the first slab by writing some zeros.
  $machine->dd(
               if    => "/dev/zero",
               of    => $device->getSymbolicPath(),
               count => 1024,
               bs    => $self->{blockSize},
               conv  => "fdatasync",
              );

  $self->recoverAndRestartVDO();
}

########################################################################
# Verify that reading and writing can happen during online recovery. Report
# how long online recovery took.
##
sub testRecovery {
  my ($self)  = assertNumArgs(1, @_);
  my $device  = $self->getDevice();
  my $machine = $device->getMachine();

  my $dataBlocks = $self->_fillVDO();
  my $vdo        = $self->getVDODevice();
  my $vdoStats   = $vdo->getVDOStats();
  $vdoStats->logStats($vdo->getDevicePath());

  my $oldSize = $vdoStats->{'physical blocks'} * $self->{blockSize};

  $log->info("Performing clean shutdown and restart VDO");
  $device->restart();

  # Dirty all the slabs by writing zeros.
  my $fioOptions
    = {
       %DEFAULT_FIO_OPTIONS,
       blockSize       => $self->{blockSize},
       filename        => $device->getSymbolicPath(),
       writePerJob     => $dataBlocks * $self->{blockSize},
       zeroBuffers     => 1,
      };
  my $fio = Permabit::CommandString::FIO->new($self, $fioOptions);
  $machine->assertExecuteCommand("($fio)");

  # Now write some data that we can read back after the crash.
  my $datasetSize = 100 * $self->{blockSize};
  my $datasetTest = $self->generateDataFile($datasetSize, "recoveryTest");
  $machine->fsync($datasetTest);
  $device->ddWrite(
                   if    => $datasetTest,
                   bs    => $self->{blockSize},
                   count => 100,
                   conv  => "fdatasync",
                   oflag => "direct",
                  );

  # Write a zero block to issue a flush.
  $device->ddWrite(
                   if    => "/dev/zero",
                   seek  => 100,
                   count => 1,
                   bs    => $self->{blockSize},
                   oflag => "direct",
                   conv  => "fdatasync",
                  );

  # Sync to make sure the VDO sticks around.
  $machine->runSystemCmd("sync");

  my $rebootCursor = $machine->getKernelJournalCursor();

  # Make sure the journal log is persisted before bouncing the system.
  $machine->syncKernLog();

  $self->recoverAndRestartVDO();

  # Verify that the old data is still there.
  $device->ddRead(
                  of    => $datasetTest . "-temp",
                  bs    => $self->{blockSize},
                  count => 100,
                 );

  # Make certain we can write during Recovery mode.
  $fioOptions
    = {
       %DEFAULT_FIO_OPTIONS,
       blockSize   => $self->{blockSize},
       filename    => $device->getSymbolicPath(),
       writePerJob => $self->{blockSize}, # one block
      };
  $fio = Permabit::CommandString::FIO->new($self, $fioOptions);
  $machine->assertExecuteCommand("($fio)");
  $vdoStats = $vdo->getVDOStats();
  assertEq("recovering", $vdoStats->{"operating mode"});
  assertLTNumeric($vdoStats->{"recovery progress (%)"}, 100);

  # Confirm that the data was read correctly.
  $machine->runSystemCmd("cmp $datasetTest ${datasetTest}-temp");

  # Former GrowPhysical03 logic: Try to grow by a slab.
  my $newSize = $oldSize + (2 ** $self->{slabBits}) * $self->{blockSize};
  eval {
    $vdo->growPhysical($newSize);
  };
  if ($device->isa("Permabit::BlockDevice::VDO::LVMManaged")) {
    assertEvalErrorMatches(qr/reload ioctl on .* failed/);
  } else {
    assertEvalErrorMatches(qr/ERROR\s*[:-] Device.*could not be changed/);
  }
  assertNENumeric(0, $machine->getStatus());
  assertTrue($machine->searchKernelJournalSince($rebootCursor,
                                                "VDO_RETRY_AFTER_REBUILD"));

  $vdo->waitUntilRecoveryComplete();

  # Calculate how long recovery took
  my $logText = $machine->getKernelJournalSince($rebootCursor);

  my $startTime = 0;
  if ($logText =~ m/\[\s*(\d+.\d+)\].*Entering recovery mode/) {
    $startTime = $1;
  } else {
    croak("recovery start time not found; perhaps recovery did not happen?");
  }

  my $endTime   = 0;
  if ($logText =~ m/\[\s*(\d+.\d+)\].*Exiting recovery mode/) {
    $endTime = $1;
  } else {
    croak("recovery end time not found; perhaps recovery did not happen?");
  }

  my $recoveryTime = $endTime - $startTime;
  $log->info("Recovery took " . $recoveryTime . " seconds.");
}

1;
