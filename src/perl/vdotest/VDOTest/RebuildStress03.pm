##
# Stress test VDO rebuild with multiple streams of operations.
#
# Must use a VDO as the topmost device.
#
# $Id$
##
package VDOTest::RebuildStress03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertFalse assertNumArgs assertTrue);
use Permabit::Constants;
use Permabit::Utils qw(getRandomElement sizeToLvmText);

use base qw(VDOTest::RebuildStressBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $EMPTY_OPS = {
                 "Generate" => 1,
                };
my $NORM_OPS = {
                "Copy"              => 0.09,
                "CompressionToggle" => 0.04,
                "EnterReadOnly"     => 0.01,
                "Generate"          => 0.09,
                "Reboot"            => 0.01,
                "Recover"           => 0.01,
                "Regenerate"        => 0.09,
                "Remove"            => 0.30,
                "Restart"           => 0.01,
                "Sleep"             => 0.10,
                "Tar"               => 0.09,
                "Verify"            => 0.15,
                "VerifyAll"         => 0.01,
               };
my $FULL_OPS = {
                "EnterReadOnly" => 0.02,
                "Quiesce"       => 0.05,
                "Recover"       => 0.05,
                "Remove"        => 0.38,
                "Restart"       => 0.05,
                "Verify"        => 0.45,
               };
my $READ_ONLY_OPS = {
                     "FullRebuild" => 0.02,
                     "Restart"     => 0.15,
                     "Sleep"       => 0.20,
                     "Verify"      => 0.60,
                     "VerifyAll"   => 0.03,
                    };

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The number of bio acknowledgement threads to use
     bioAckThreadCount   => 2,
     # @ple The number of bio submission threads to use
     bioThreadCount      => 2,
     # @ple The amount of block map cache to use
     blockMapCacheSize   => 256 * $MB,
     # @ple The number of "CPU" (hashing etc) threads to use for a VDO device
     cpuThreadCount      => 2,
     # @ple Don't enable compression initially.
     enableCompression   => 0,
     # @ple Number of hash lock threads/zones to use
     hashZoneThreadCount => 2,
     # @ple Number of logical threads/zones to use
     logicalThreadCount  => 2,
     # @ple Number of physical threads/zones to use
     physicalThreadCount => 2,
     # @ple The type of stress test (true -> parallel, false -> serial)
     stressParallel      => 1,
    );
##

########################################################################
# @inherit
##
sub getTable {
  my ($self) = assertNumArgs(1, @_);

  my $vdoStats = $self->getDevice()->getCurrentVDOStats();
  if ($vdoStats->{"operating mode"} eq "read-only") {
    assertTrue($self->{readOnly});
    $log->info("Using read-only operations");
    return $READ_ONLY_OPS;
  }
  assertFalse($self->{readOnly});

  if (scalar(@{$self->{datasets}}) < 2) {
    $log->info("Using empty operations");
    return $EMPTY_OPS;
  }

  my $availableBlocks = $self->getAvailableBlocks();
  if ($availableBlocks <= $self->{blocksPerDataset}) {
    $log->info("Using full table ($availableBlocks blocks free)");
    return $FULL_OPS;
  }

  $log->info("Using normal operations");
  return $NORM_OPS;
}

########################################################################
# @inherit
##
sub checkFinalStressTestState {
  my ($self) = assertNumArgs(1, @_);

  if (defined($self->{readOnly}) && $self->{readOnly}) {
    # Get back into the expected "normal" operating state.
    $self->doFullRebuild();
  }

  $self->SUPER::checkFinalStressTestState();
}

########################################################################
# Toggle the VDO's compression status.
##
sub doCompressionToggle {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  if ($self->{enableCompression}) {
    $self->{enableCompression} = 0;
    $device->disableCompression();
  } else {
    $self->{enableCompression} = 1;
    $device->enableCompression();
  }
}

########################################################################
# Restart VDO in read-only mode.
##
sub doEnterReadOnly {
  my ($self) = assertNumArgs(1, @_);
  $self->doQuiesce();
  my $fs = $self->getFileSystem();
  $fs->unmount();
  $self->getDevice()->setReadOnlyMode();
  # Remount read-only
  $fs->mount(1);
  $self->{readOnly} = 1;
}

########################################################################
# Restart a read-only VDO and do a full rebuild.
##
sub doFullRebuild {
  my ($self) = assertNumArgs(1, @_);
  $self->doQuiesce();
  my $fs = $self->getFileSystem();
  $fs->unmount();
  my $device = $self->getDevice();
  $device->stop();
  $device->forceRebuild();
  $fs->mount();
  $self->{readOnly} = 0;
}

########################################################################
# @inherit
##
sub doReboot {
  my ($self) = assertNumArgs(1, @_);
  $self->doQuiesce();
  $self->doThreadCountChange();
  $self->recoverAndRestartVDO();
}

########################################################################
# @inherit
##
sub doRestart {
  my ($self) = assertNumArgs(1, @_);
  $self->doThreadCountChange();
  $self->SUPER::doRestart();
}

########################################################################
# Change the number of threads used by the VDO.
##
sub doThreadCountChange {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  # XXX when LVM adds support for changing thread counts (BZ 2070777), remove
  # this and implement properly at the bottom.
  if ($device->isa("Permabit::BlockDevice::VDO::LVMVDO::Managed")) {
    $log->info("Skipping thread count change on Managed device");
    return;
  }

  $log->info("Changing thread counts (will take effect next reboot/restart)");

  my @threadCountFields
    = map { $_ . "ThreadCount" } qw(bioAck bio cpu hashZone logical physical);
  for my $threadCountKey (@threadCountFields) {
    my $threadCount = $self->{$threadCountKey};
    my $delta = getRandomElement([-1, 0, 1]);
    # Never adjust a number below 1 or above 16.
    if ($threadCount == 1) {
      $delta++;
    } elsif ($threadCount == 16) {
      $delta--;
    }
    $self->{$threadCountKey} += $delta;
  }

  # Grow the cache (never shrinking) as needed.
  my $requiredBlockMapCache = 128 * $MB * $self->{logicalThreadCount};
  if ($requiredBlockMapCache > $self->{blockMapCacheSize}) {
    $self->{blockMapCacheSize} = $requiredBlockMapCache;
  }

  my $modifyArgs = {
    blockMapCacheSize  => sizeToLvmText($self->{blockMapCacheSize}),
    vdoAckThreads      => $self->{bioAckThreadCount},
    vdoBioThreads      => $self->{bioThreadCount},
    vdoCpuThreads      => $self->{cpuThreadCount},
    vdoHashZoneThreads => $self->{hashZoneThreadCount},
    vdoLogicalThreads  => $self->{logicalThreadCount},
    vdoPhysicalThreads => $self->{physicalThreadCount},
  };
  $device->assertVDOCommand("modify", $modifyArgs);
}

1;
