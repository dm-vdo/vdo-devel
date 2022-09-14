##
# Base class for testing kvdo performance via fio
#
# $Id$
##
package VDOTest::VDOFIOPerfTestBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Carp;
use Log::Log4perl;
use Permabit::AlbireoTestUtils qw(getAlbGenConfigFile);
use Permabit::Assertions qw(assertMinMaxArgs assertNumArgs);
use Permabit::CommandString::FIO;
use Permabit::Constants;
use Permabit::LabUtils qw(getTotalRAM);
use Permabit::Utils qw(parseBytes);
use POSIX qw(ceil);

use base qw(VDOTest::VDOPerfBase);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);
my $fioInvocation = 0;

# FIO properties for tests descended from this one
our %FIO_TEST_PROPERTIES
  = (
     # @ple if set, fio will fill the IO buffers with this pattern
     bufferPattern        => undef,
     # @ple if a compress percent is set, then we will either use regular fio
     # or albGenStream to generate data
     compressPercent      => undef,
     # @ple CPU core IDs that are allowed for use in threads, used to lock
     #      cores
     cpusAllowed          => undef,
     # @ple If a dedupe percent is set, then we will use use albGenTest to
     #      generate the data
     dedupePercent        => undef,
     # @ple use hardware raid by default
     directIo             => 1,
     # @ple the blockSize fio should use
     fioBlockSize         => 4 * $KB,
     # @ple how often (after how many I/O operations) fio should fsync
     fioFsync             => undef,
     # @ple number of pending IOs. (128 was recommended by other vendors)
     ioDepth              => 128,
     # @ple minimum number of IOs at once to ask for during the retrieval
     #      process from the kernel. default is 1 which means the number of IOs
     #      retrieved will be between 1 and ioDepth.
     ioDepthBatchComplete => undef,
     # @ple pending Number of IOs to submit at once.
     ioDepthBatchSubmit   => undef,
     # @ple the "way" that fio writes/reads its blocks.
     ioEngine             => "libaio",
     # @ple the amount of data to write (if defaulting to the logical size
     #       is not desired)
     ioSize               => undef,
     # @ple fio IO pattern (read,write,randread,randwrite); Default is
     #      randwrite.
     ioType               => "randrw",
     # @ple number of jobs to use
     jobCt                => 5,
     # @ple log latency info
     latencyTracking      => 1,
     # @ple fio iteration count
     loops                => undef,
     # @ple consume all but n bytes of memory on the system. The size of the
     #      Albireo index is automatically accounted for. (undef means no
     #      mlocking)
     mlock                => undef,
     # @ple do not try to hit every block with random I/O
     norandommap          => 1,
     # @ple the offset at which to start reading/writing
     offset               => 0,
     # @ple the offset for each subjob
     offsetIncrement      => undef,
     # @ple If a write-only test, whether to prewrite data. Tests involving
     #      reads always prewrite, since reading unwritten blocks is
     #      unrealistically fast. Tests involving writes are faster when
     #      the VDO starts out empty; steady state performance is better
     #      reflected when the volume is prewritten. Truthy values cause
     #      prewriting, undef and false values don't.
     preWriteData         => undef,
     # @ple Use repeatable random sequence in test I/O
     randrepeat           => 1,
     # @ple Bandwidth limit in bytes/second; applies to both read and write
     #      rates unless specified separately.  Uses same format as "rate"
     #      parameter for FIO: 1000000, 1000k, and 1m are the same amount.
     #      Separate read/write can be specified like this: "1m,500k" would
     #      limit read rate to 1 MB/second and write rate to 500 KB/second.
     rate                 => undef,
     # @ple Seconds to run for.
     runTime              => undef,
     # @ple Percentage of a mixed workload that should be reads
     rwmixread            => undef,
     # @ple Use softrandommap for random workloads
     softrandommap        => undef,
     # @ple If true, use the random number generator from the OS; otherwise use
     #      FIO's own generator.
     useOsRand            => undef,
     # @ple bytes per thread for fio to read and/or write (depending on ioType)
     writePerJob          => 500 * $MB,
     # @ple if set, fio will fill the IO buffers with zeroes
     zeroBuffers          => undef,
    );

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     %FIO_TEST_PROPERTIES,
     # @ple default to computing logicalSize below
     logicalSize          => undef,
    );
##

###############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);

  # Translate user input into bytes
  foreach my $param (qw(fioBlockSize writePerJob mlock ioSize)) {
    if (defined($self->{$param})) {
      $self->{$param} = parseBytes($self->{$param});
    }
  }

  if (!defined($self->{logicalSize})) {
    # Make the logical volume large enough to fit all the data
    #  we will write.
    if ($self->{writePerJob}) {
      my $requiredSize = ($self->{jobCt} * $self->{writePerJob});
      # Allocate slack space on the vdo device for FS metadata
      $self->{logicalSize} = ceil($requiredSize * 1.05);
      # Round up to a multiple of the block size.
      $self->{logicalSize} += $self->{blockSize} - 1;
      $self->{logicalSize} -= ($self->{logicalSize} % $self->{blockSize});
      $log->debug("setting logicalSize to $self->{logicalSize}");
    }
  } else {
    # We could sanity-check the value (big enough, multiple of block
    # size, etc), but shouldn't override it.  Assume the user knows
    # what he's doing.
    $log->debug("using defined logicalSize of $self->{logicalSize}");
  }

  # If no offsets are specified, make the writes non-overlapping.
  $self->{offsetIncrement} //= $self->{writePerJob};

  if (($self->{ioType} eq 'read') || ($self->{ioType} eq 'randread')) {
    # Albireo is out of the path of the performance test, so filling
    # the index is a waste of time.
    $self->{albFill} = 0;
  }

  # enable compression for compress perf tests
  if (defined($self->{compressPercent})) {
    $self->{enableCompression} = 1;
  }

  $self->SUPER::set_up();

  # Adjust the mlock amount based on the configured Albireo Index
  if (defined($self->{mlock})
      && $self->getDevice()->isa("Permabit::BlockDevice::VDO")) {
    $self->{mlock} += $self->applicationMemoryNeeded();
  }
}

###############################################################################
# Decide whether to prewrite for this test.
#
# If a filesystem is used in performing the test, or if the test is a pure
# write test, or if the device is not a VDO, the pre-writing step will be
# skipped. For filesystems, FIO itself will write the data first to
# files before executing the testing phase. For pure write tests, we currently
# only do empty-VDO write tests. And for non-VDO tests, prewriting shouldn't
# have much effect.
#
# @return true iff the parameters indicate prewriting should be performed.
##
sub _shouldPrewrite {
  my ($self) = assertNumArgs(1, @_);
  # Never prewrite for filesystem or non-VDO tests, where it has no effect.
  if ($self->{useFilesystem}
      || !$self->getDevice()->isa("Permabit::BlockDevice::VDO")) {
    return 0;
  }

  # If the ioType only makes sense if we prewrite, do so.
  if (($self->{ioType} eq 'read')
      || ($self->{ioType} eq 'randread')
      || ($self->{ioType} eq 'rw')
      || ($self->{ioType} eq 'randrw')) {
    return 1;
  }

  # For pure write tests, prewrite iff preWriteData is true.
  return $self->{preWriteData};
}

###############################################################################
# Pre-write to the test's logical device in preparation for performing
# the test. Use sequential or random write operation that covers all
# logical blocks which will be used in the primary test operations.
#
#
# This fills the block map with actual physical blocks instead of
# zero-blocks, resulting in a true read test instead of a
# zero-block-read test; similarly, write tests will test overwriting
# real data (and thus perhaps decrementing reference counts) instead
# of overwriting blank storage. This is performed outside of the
# actual benchmarking period but against the same device, Albireo
# index, etc.
#
# @oparam random     Whether to use random-access or sequential writes
##
sub preWriteData {
  my ($self, $random) = assertMinMaxArgs(1, 2, @_);
  if (!$self->_shouldPrewrite()) {
    return;
  }

  # In setting the FIO parameters used for pre-writing, most of the
  # option values are the same as in the actual benchmark invocation.
  # This includes the "writePerJob", "offset", "offsetIncrement", and "jobs"
  # settings.  These are left unchanged so that the pre-writing exactly
  # matches the regions of the logical device which will be read, which may
  # be non-contiguous or have other unusual placement.  The jobs value
  # must also be the same for dedupe tests to match up with the
  # albgenstream configuration correctly.
  my $preWriteFixedOptions = {
    ioEngine      => "psync",
    ioType        => $random ? "randwrite" : "write",
    directIo      => 0,
    ioDepth       => 1,
    loops         => 1,
    mlock         => undef,
    norandommap   => 1,
    randrepeat    => undef,
    rate          => undef,
    runTime       => undef,
    softrandommap => undef,
    useOsRand     => undef,
  };

  my $fioOptions = $self->extractFioBenchmarkOptions($preWriteFixedOptions);

  my $fioCommand = Permabit::CommandString::FIO->new($self, $fioOptions);
  $log->info("Pre-writing data to logical device before performing "
             . "benchmark");
  $self->getDevice()->getMachine()->assertExecuteCommand("($fioCommand)");

  # Make sure all resources used by prewriting have been returned, so the main
  # portion of the test is unaffected.
  $self->getDevice()->doVDOSync();
}

###############################################################################
# From a set of fio test options, or from $self if none is provided, generate
# a set of FIO test parameters.
#
# @return   a hashref of fio options
##
sub extractFioBenchmarkOptions {
  my ($self, $extraOptions) = assertMinMaxArgs([{}], 1, 2, @_);
  my %testOpts = (
                     (map { $_ => $self->{$_} } keys(%FIO_TEST_PROPERTIES)),
                     %$extraOptions,
                    );
  my $testOptions = \%testOpts;
  my %benchOpts =
    (
     blockSize            => $testOptions->{fioBlockSize},
     compressPercent      => $testOptions->{compressPercent},
     cpusAllowed          => $testOptions->{cpusAllowed},
     directIo             => $testOptions->{directIo},
     endFsync             => 1,
     fsync                => $testOptions->{fioFsync},
     ioDepth              => $testOptions->{ioDepth},
     ioDepthBatchComplete => $testOptions->{ioDepthBatchComplete},
     ioDepthBatchSubmit   => $testOptions->{ioDepthBatchSubmit},
     ioEngine             => $testOptions->{ioEngine},
     ioSize               => $testOptions->{ioSize},
     ioType               => $testOptions->{ioType},
     jobs                 => $testOptions->{jobCt},
     latency              => $testOptions->{latencyTracking},
     loops                => $testOptions->{loops},
     norandommap          => $testOptions->{norandommap},
     offset               => $testOptions->{offset},
     offsetIncrement      => $testOptions->{offsetIncrement},
     randrepeat           => $testOptions->{randrepeat},
     rate                 => $testOptions->{rate},
     runTime              => $testOptions->{runTime},
     rwmixread            => $testOptions->{rwmixread},
     softrandommap        => $testOptions->{softrandommap},
     useOsRand            => $testOptions->{useOsRand},
     writePerJob          => $testOptions->{writePerJob},
    );

  my $streamSize = $testOptions->{writePerJob};
  my $dp = $testOptions->{dedupePercent} // 0;
  # Make sure to use the albgenstream code in fio when doing compress. fio
  # generating its own compression data takes too long.
  if (defined($self->{dedupePercent}) || defined($self->{compressPercent})) {
    # For read-write dedupe tests, the stream size must be reduced to the
    # actual number of bytes each job will write so that the dedupe
    # percentage in the workload is correct for all bytes actually written.
    if (($testOptions->{ioType} eq 'rw')
        || ($testOptions->{ioType} eq 'randrw')) {
      $streamSize = int(($streamSize * (100 - $testOptions->{rwmixread})) / 100);
      $streamSize -= $streamSize % $testOptions->{fioBlockSize};
    }
    $benchOpts{albGenStream} = getAlbGenConfigFile($streamSize,
                                                   $dp,
                                                   $testOptions->{fioBlockSize},
                                                   "Data$fioInvocation");
  }
  $fioInvocation++;

  if ($self->{useFilesystem}) {
    $benchOpts{directory} = $self->getFileSystem()->getMountDir();
    # we create a file per job, so its already offset properly on disk.
    $benchOpts{offsetIncrement} = 0;
    $benchOpts{cleanupBenchmark} = 1;
  } else {
    $benchOpts{filename} = $self->getDevice()->getDevicePath();
    $benchOpts{cleanupBenchmark} = 0;
  }
  if (defined($testOptions->{bufferPattern})) {
    $benchOpts{bufferPattern} = $testOptions->{bufferPattern};
  }
  if (defined($testOptions->{zeroBuffers})) {
    $benchOpts{zeroBuffers} = $testOptions->{zeroBuffers};
  }

  if ($testOptions->{mlock}) {
    my $machine = $self->getDevice()->getMachine();
    my $totalMem = getTotalRAM($machine->getName());
    $benchOpts{lockMem} = $totalMem - $testOptions->{mlock};
    if ($benchOpts{lockMem} < 0) {
      croak($machine->getName()
            . " doesn't have enough memory to reserve requested "
            . $testOptions->{mlock} . " bytes out of $totalMem bytes total");
    }
  }

  return \%benchOpts;
}

1;
