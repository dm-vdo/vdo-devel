##
# One big honking test to measure performance in a bunch of different
# fio test configurations, multiple times with averaging.
#
# This is not a normal "perf" test in the sense of something we want
# to run regularly that comes up with one number to monitor our
# progress (or regression) over time.
#
# The log file format for reporting performance statistics is known by
# the "compareMongoResults" script, including the fact that
# "cpusAllowed" is the alphabetically-first test parameter configured,
# so if you change the test parameters or logging format, that script
# may need updating.
#
# Steady state logic: The recovery journal holds about 10 million entries.
# The block map cache and the slab journals start writing out after 5
# million entries. Slabs get reused when more than half empty, so we want
# to fill at least half the physical space so we're no longer using empty
# slabs. Thus the prewriting should fulfill these conditions:
# 1. > 2.5 million writes, 10G.
# 2. At least half the physical space is full.
# 3. Filled via randwrite across the logical space so the cache is full.
# 4. No dedupe/compress.
#
# If we do N randwrites on X logical blocks, all unique, we expect
# X(1-(1-(1/X))^N) logical blocks (and thus physical blocks) to be used.
# Suppose we do exactly 11G writes (for some slack) and would like this
# to fill 3/4ths the space; then we need (1-(1/X))^2750000=.25; or 
# X = 1/(1-e^((ln .25)/2750000)); or 2000000 logical blocks.
#
# Or, for my favorite config: 12G physical, 2G slabs -> 2.6 million
# physical blocks; logical size of 15G => 3.9 million logical blocks.
# Then 10G writes will fill 1.9 million blocks out of the available
# 2.6 million blocks, and an additional 4G writes will bring that
# number up to 2.4 million blocks, thus making sure there's plenty
# of free space still.
#
# To do:
#
#  * Constrain the physical size. Otherwise, overwrites will allocate
#    new storage from empty slabs, expanding the number of slabs we
#    have to update ref counts or read data from on as time goes by
#    and reducing locality. (Not relevant if you only run randread
#    tests.)
#
#  * Do more writing in the initialization (prewrite) phase, if we're
#    including any write tests, so that we move into the stage of
#    recycling available storage from previously-used slabs, and the
#    physical blocks used are well distributed through the available
#    storage. We want our VDO device to reach a steady state with
#    regard to block distribution before we start any tests.
#
# $Id$
##
package VDOTest::Mongo;

use strict;
use warnings FATAL => qw(all);
use Carp;
use Data::Dumper;
use English qw(-no_match_vars);
use List::Util qw(max shuffle);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEqualNumeric
  assertMinArgs
  assertNumArgs
  assertTrue
);
use Permabit::CommandString::FIO;
use Permabit::Constants;
use Permabit::FIOUtils qw(runFIO);
use Permabit::Statistic;

use base qw(VDOTest::VDOFIOPerfTestBase);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The vectors of test property values to test across. Can be FIO
     #      options, or, if reformatEachTest is set, VDO options.
     testVectors          => {
		              dedupePercent   => [0, 50, 90],
			      compressPercent => [0, 55, 90],
                              ioDepth         => [1024],
                              ioType          => ["randwrite"],
                              jobCt           => [4],
			      writePerJob     => [$GB],
			      offsetIncrement => [0],
                             },
     # @ple The number of bio acknowledgement threads to use
     bioAckThreadCount    => 3,
     # @ple The number of bio submission threads to use
     bioThreadCount       => 6,
     # @ple The amount of memory allocated for cached block map pages
     blockMapCacheSize    => "4g",
     # @ple The number of "CPU" (hashing etc) threads to use for a VDO device
     cpuThreadCount       => 2,
     # @ple turn this off (likely because they're graphing)
     gtod_reduce          => 0,
     # @ple Number of hash lock threads/zones to use
     hashZoneThreadCount  => 1,
     # @ple start with a clean index
     indexPre             => 'clean',
     # @ple number of pending IOs. (128 was recommended by other vendors)
     ioDepth              => 128,
     # @ple minimum number of IOs at once to ask for during the retrieval
     #      process from the kernel. default is 1 which means the number of IOs
     #      retrieved will be between 1 and ioDepth.
     # XXX Disabled due to VDO-4533 till we rebase FIO
     # ioDepthBatchComplete => 16,
     # @ple pending Number of IOs to submit at once.
     ioDepthBatchSubmit   => 16,
     # @ple the "way" that fio writes/reads its blocks.
     ioEngine             => "libaio",
     # @ple the "way" that fio writes/reads its blocks.
     ioType               => "randwrite",
     # @ple How many times to run each test configuration
     iterationCount       => 10,
     # @ple log latency info
     latencyTracking      => 1,
     # @ple default to computing logicalSize below
     logicalSize          => undef,
     # @ple Number of logical threads/zones to use
     logicalThreadCount   => 4,
     # @ple Number of physical threads/zones to use
     physicalThreadCount  => 1,
     # @ple The test phase may contain read tests, so prewrite data before
     #      testing.
     preWriteData         => 1,
     # @ple whether to re-initialize (including potentially prewriting) before
     #      each iteration
     reformatEachTest     => 1,
     # @ple the number of bits in the VDO slab
     slabBits             => $SLAB_BITS_TINY,
     # @ple use threads instead of procs
     thread               => 1,
     # @ple don't create a fileSystem
     useFilesystem        => 0,
     # @ple Comma-separated list of CPUs on which to run VDO threads
     #vdoAffinityList      => "0-7,bio\@8-15",
     # @ple bytes per thread for fio to read and/or write (depending on ioType)
     writePerJob          => 10 * $GB,
    );
##

###############################################################################
# Force interrupts for the OCZ controllers to be tied to specific
# cores.
##
sub adjustOCZInterrupts {
  my ($self) = assertNumArgs(1, @_);
  return;
  my $machine = $self->getUserMachine();
  if (defined($self->{clientClass}) && $self->{clientClass} =~ m/OCZ/) {
    $machine->runSystemCmd("/etc/init.d/irqbalance stop");
    my @irqs = $machine->getInterruptsByName("oczpcie");
    $log->debug("OCZ IRQs: " . join(", ", @irqs));
    $self->{_oczIRQs} = \@irqs;
    # Pick cores all on node 1.
    my $numaMap = $machine->getNUMAMap();
    my @node1cores = @{$numaMap->[1]};
    my $numIRQs = scalar(@irqs);
    my @irqCores = @node1cores[0..($numIRQs-1)];
    assertEqualNumeric(scalar(@irqCores), scalar(@irqs));
    my @allCores = map { @{$_} } @{$numaMap};
    $self->{_maxCoreNumber} = max(@allCores);
    foreach my $irq (@irqs) {
      my $core = shift(@irqCores);
      $machine->setIRQAffinity($irq, $core);
    }
    # Maybe run irqbalance -i79 -i80 ...?
  }
}

###############################################################################
# Remove the OCZ interrupt affinity setting.
##
sub restoreOCZInterrupts {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{_oczIRQs} && $self->{_maxCoreNumber}) {
    my $machine = $self->getUserMachine();
    my $allCores = "0-$self->{_maxCoreNumber}";
    foreach my $irq (@{$self->{_oczIRQs}}) {
      $machine->setIRQAffinity($irq, $allCores);
    }
    $machine->runSystemCmd("/etc/init.d/irqbalance start");
  }
}

###############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();
  $self->adjustOCZInterrupts();
}

###############################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  # Restore normal IRQ handling
  $self->runTearDownStep(sub { $self->restoreOCZInterrupts(); });
  $self->SUPER::tear_down();
}

###############################################################################
# Generate the full Cartesian product of the supplied arrays.
#
# @param hashrefs  Hash ref mapping labels to lists of values
#
# @return  array of hashrefs, each mapping labels to individual values
##
sub _makeHashes {
  my (%hashrefs) = assertMinArgs(1, @_);
  my @labels = keys(%hashrefs);
  my @results = ( {} );
  foreach my $label (@labels) {
    my @values = @{$hashrefs{$label}};
    my @newresults = map {
      my $newValue = $_;
      map {
        # Make a new hash adding in the new label/value.
        { $label => $newValue, %{$_} }
      } @results;
    } @values;
    @results = @newresults;
  }
  return @results;
}

###############################################################################
# Generate a list of fio test parameter configurations.
#
# @return  array of hashrefs of fio parameter settings
##
sub getParameterSets {
  my ($self) = assertNumArgs(1, @_);
  # How many of the ${fioConfigOption}s are set? Find them and build a 
  # hash of config parameters.
  my %configParameters = %{$self->{testVectors}};
  foreach my $parameter (keys(%configParameters)) {
    assertTrue(!defined($configParameters{$parameter})
               || ref($configParameters{$parameter}) eq 'ARRAY',
                 "$parameter must have an array of values");
  }

  my @configs = _makeHashes(%configParameters);
  # If the total outstanding I/Os number over 10K, we're just being
  # silly. Actually the silliness threshold should probably be lower
  # than that.
  if (defined($configParameters{jobCt})
      && defined($configParameters{ioDepth})) {
    @configs = grep { $_->{jobCt} * $_->{ioDepth} < 10000; } @configs;
  }
  # Repeated runs, scrambled order.
  @configs = shuffle((@configs) x $self->{iterationCount});
  $log->info("total " . scalar(@configs) . " tests to run");
  $log->info("shuffled test set: "
             . (join(", ", map { Dumper($_) } @configs)));
  return @configs;
}

###############################################################################
# Run an assortment of test configurations in a big loop.
##
sub testEverythingOverAndOver {
  my ($self) = assertNumArgs(1, @_);
  my @configs = $self->getParameterSets();

  my $machine = ($self->{useFilesystem}
                 ? $self->getFileSystem()->getMachine()
                 : $self->getDevice()->getMachine());
  # iterate over loop
  my $count = 0;
  my @labels = sort(keys(%{$configs[0]}));
  my %savedRates;
  my %statsCounters;
  foreach my $config (@configs) {
    if ($self->{reformatEachTest} || ($count == 0)) {
      # prewrite
      $self->destroyTestDevice($self->getDevice());
      $self->createTestDevice($self->{deviceType}, %$config);
      $self->preWriteData(1);
    }
    $count++;
    my $configString = join(",",
                            map { $_ . "=" . $config->{$_} } @labels);
    $log->info("pass $count : $configString");
    $savedRates{$configString} ||= ();
    $statsCounters{$configString} ||= Permabit::Statistic->new();

    my $fioOpts = $self->extractFioBenchmarkOptions($config);
    $log->debug("test options: " . Dumper($config));

    my $fioCommand = Permabit::CommandString::FIO->new($self, $fioOpts);
    my $results = runFIO($machine, $fioCommand);
    $log->debug("results: " . Dumper($results));
    my $rate = $results->{read}->{rate} + $results->{write}->{rate};
    my $size = $results->{read}->{bytes} + $results->{write}->{bytes};
    $log->info("pass $count result: $configString rate = $rate over $size -- "
               . ($rate/$MB) . " MB/s, "
               . ($size/$MB) . " MB");
    push(@{$savedRates{$configString}}, $rate);
    $statsCounters{$configString}->sample($rate);
    # If that was a write test, we don't want post-acknowledgement
    # processing (dedupe, compression) in sync mode to interfere
    #  with the next test. In async mode, the endFsync setting on
    #  fio will have caused a flush, which will have already waited
    #  until all post-acknowledgement processing is complete.
    if (!$self->{reformatEachTest} && ($config->{ioType} =~ m/write|rw/)) {
      $self->getDevice()->doVDOSync();
    }
  }

  $log->info("summary:");
  foreach my $configString (sort(keys(%savedRates))) {
    my @rates = @{$savedRates{$configString}};
    $log->info("$configString : " . join(" ", sort {$a <=> $b} @rates));
    my $stat = $statsCounters{$configString};
    my $mean = $stat->mean();
    my $sampleStandardDev = -1;
    if ($stat->count() > 1) {
      $sampleStandardDev = $stat->sigma();
    }
    my $cv = $sampleStandardDev * 100 / $mean;
    my $meanMB = $mean / $MB;
    my $ssdMB = $sampleStandardDev / $MB;
    $log->info("$configString : mean $meanMB MB/s ssd $ssdMB MB/s cv\% $cv");
  }
}
