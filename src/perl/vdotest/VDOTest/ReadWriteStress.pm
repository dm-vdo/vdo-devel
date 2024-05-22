##
# This is a stress test using multiple fio processes to try to produce unusual
# VIO combinations.
#
# $Id$
##
package VDOTest::ReadWriteStress;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::AlbireoTestUtils qw(getAlbGenConfigFile);
use Permabit::Assertions qw(assertNumArgs);
use Permabit::AsyncTask::LoopRunSystemCmd;
use Permabit::CommandString::FIO;
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple How often to rotate between bio sumission threads
     bioThreadRotationalInterval => 32,
     # @ple Number of bio submission threads to use
     bioThreadCount              => 2,
     # @ple Number of bio acknowledgment threads to use
     bioAckThreadCount           => 2,
     # @ple use a larger cache size
     blockMapCacheSize           => $GB,
     # @ple use SSD machines
     clientClass                 => "VDO-PMI",
     # @ple Number of CPU threads to use
     cpuThreadCount              => 2,
     # @ple The type of VDO device to use
     deviceType                  => "lvmvdo",
     # @ple Enable offset writes
     emulate512Enabled           => 1,
     # @ple Enable compression
     enableCompression           => 1,
     # @ple Number of hash zone threads to use
     hashZoneThreadCount         => 2,
     # @ple The size of the logical space
     logicalSize                 => 6 * $GB,
     # @ple Number of logical threads to use
     logicalThreadCount          => 2,
     # @ple The size of the underlying physical device
     physicalSize                => 20 * $GB,
     # @ple Number of physical threads to use
     physicalThreadCount         => 2,
     # @ple The list of active tasks
     _tasks                      => [],
     # @ple The amount of data to read or write per thread.
     writePerJob                 => 5 * $GB,
    );
##

#############################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  foreach my $task (@{$self->{_tasks}}) {
    # Use SIGKILL to ensure that fio terminates
    $task->kill('SIGKILL');
  }

  $self->SUPER::tear_down();
}

#############################################################################
# Test for race conditions.
##
sub testReadDedupeCompress {
  my ($self) = assertNumArgs(1, @_);
  my $devicePath = $self->getDevice()->getSymbolicPath();
  my $machine = $self->getDevice()->getMachine();

  # Use a configuration for 60% dedupe
  my $dedupeConfigFile = getAlbGenConfigFile($self->{writePerJob}, 60,
                                             $self->{blockSize}, "Dedupe");
  my $dedupeOptions
    = {
       jobName              => 'dedupeStream',
       albGenStream         => $dedupeConfigFile,
       blockSize            => $self->{blockSize},
       directIo             => 1,
       cleanupBenchmark     => 0,
       filename             => $devicePath,
       ioType               => "randwrite",
       ioEngine             => 'libaio',
       ioDepth              => 256,
       # XXX Disabled due to VDO-4533 till we rebase FIO.
       #ioDepthBatchSubmit   => 16,
       ioDepthBatchComplete => 16,
       gtod_reduce          => 0,
       group_reporting      => 1,
       randrepeat           => 1,
       thread               => 1,
       jobs                 => 2,
       offset               => 10240,
       offsetIncrement      => 10240,
       scrambleBuffers      => 0,
      };
  my $fioDedupe = Permabit::CommandString::FIO->new($self, $dedupeOptions);

  my $compressOptions
    = {
       jobName          => 'compressStream',
       blockSize        => $self->{blockSize},
       directIo         => 1,
       compressPercent  => 94,
       cleanupBenchmark => 0,
       filename         => $devicePath,
       ioType           => "randwrite",
       ioEngine         => 'libaio',
       ioDepth          => 128,
       gtod_reduce      => 0,
       norandommap      => 1,
       randrepeat       => 1,
       scrambleBuffers  => 1,
       thread           => 1,
       jobs             => 4,
       writePerJob      => $self->{writePerJob},
      };
  my $fioCompress = Permabit::CommandString::FIO->new($self, $compressOptions);

  my $readOptions
    = {
       jobName          => 'readStream',
       blockSize        => $self->{blockSize},
       directIo         => 1,
       cleanupBenchmark => 0,
       filename         => $devicePath,
       ioType           => "randread",
       ioEngine         => 'libaio',
       ioDepth          => 128,
       gtod_reduce      => 0,
       norandommap      => 1,
       thread           => 1,
       jobs             => 2,
       writePerJob      => $self->{writePerJob},
      };
  my $fioRead = Permabit::CommandString::FIO->new($self, $readOptions);

  my $compressTask
    = Permabit::AsyncTask::LoopRunSystemCmd->new($machine, "Compress", 100,
                                                 "($fioCompress)");
  my $dedupeTask
    = Permabit::AsyncTask::LoopRunSystemCmd->new($machine, "Dedupe", 100,
                                                 "($fioDedupe)");
  my $readTask
    = Permabit::AsyncTask::LoopRunSystemCmd->new($machine, "Read", 100,
                                                 "($fioRead)");

  $self->getAsyncTasks()->addTasks($compressTask, $dedupeTask, $readTask);
  $self->getAsyncTasks()->finish();
}

1;
