##
# Test how memory usage scales when scaling the logical and physical size
# of the VDO.
#
# $Id$
##
package VDOTest::MemoryMeasurement;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertNear
  assertNumArgs
);
use Permabit::Constants;
use Permabit::SystemUtils qw(assertSystem);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

# These constants were observed, if anything changes, they should be updated.
my $EXPECTED_PHYSICAL_SIZE_SCALING = 0.00025559775531292;
my $EXPECTED_CACHE_SCALING = 1.105532169;

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Run on a host which is big enough to have 6 full sized slabs.
     clientClass          => "ALBIREO-PMI",
     # @ple Use a linear device so that we can set the physical size
     deviceType           => "linear",
     # @ple Use a logical size equal to the largest physical size
     logicalSize          => 192 * $GB,
     # @ple Set physical sizes in test
     physicalSize         => undef,
     # @ple Use no more than 2 physical threads
     physicalThreadCount  => 2,
     # @ple Use maximal slabs
     slabBits             => 23,
    );
##

########################################################################
# Test how memory usage scales when scaling the logical size of a VDO.
##
sub testMemoryScaling {
  my ($self)        = assertNumArgs(1, @_);
  my $usage         = {};
  for (my $i = 1; $i < 9; $i *= 2) {
    $self->{blockMapCacheSize}           = $i * 128 * $MB;
    $usage->{$self->{blockMapCacheSize}} = {};
    for (my $vdoSlabs = 3; $vdoSlabs < 6; $vdoSlabs++) {
      # Give the test an extra GB past an whole number of slabs, to account
      # for non-slab metadata (journal, etc.). This test needs no more than
      # 2 physical threads; the starting value of $vdoSlabs should be upped
      # if the test is run with more. The test needs to explcitly resize
      # the logical device to get the right VDO size.
      $self->{physicalSize} = 32 * $GB * $vdoSlabs + 1 * $GB;
      my $preFree = _getMemoryInUse();

      # This will automatically adjust the size of the logical device
      my $vdo   = $self->createTestDevice("lvmvdo");
      my $free  = _getMemoryInUse() - $preFree;
      my $stats = $vdo->getVDOStats();
      $usage->{$self->{blockMapCacheSize}}{$self->{physicalSize}}
        = [$stats->{'KVDO module peak bytes used'}, $free];
      $self->destroyTestDevice($vdo);
    }
  }

  my @results = ('raw results:',
                 'cacheSize,physicalSize,peakMemory,freeDelta');
  my $cacheSizeDifferences = {};
  foreach my $cacheSize (sort {$a <=> $b} keys(%{$usage})) {
    my $cacheSizeResults = $usage->{$cacheSize};
    my $previous;
    foreach my $physicalSize (sort {$a <=> $b} keys(%{$cacheSizeResults})) {
      my $results = $cacheSizeResults->{$physicalSize};
      push(@results, join(',', $cacheSize, $physicalSize, @{$results}));
      my $currentUsage = $results->[0];
      if (defined($previous)) {
        my $usageScaling = (($currentUsage - $previous->[0])
                            / ($physicalSize - $previous->[1]));
        assertNear($EXPECTED_PHYSICAL_SIZE_SCALING, $usageScaling, 0.0001,
                  "physical size scaling");
      }
      $previous = [$currentUsage, $physicalSize];
      my $cacheSizePrevious = $cacheSizeDifferences->{$physicalSize}{previous};
      if (defined($cacheSizePrevious)) {
        my $usageScaling = (($currentUsage - $cacheSizePrevious->[0])
                            / ($cacheSize - $cacheSizePrevious->[1]));
        assertNear($EXPECTED_CACHE_SCALING, $usageScaling, .005,
                   "cache scaling");
      }
      $cacheSizeDifferences->{$physicalSize}{previous}
        = [$currentUsage, $cacheSize];
    }
  }

  $log->info(join("\n", @results));
}

########################################################################
# Get the memory in use according to the free command.
#
# @return The free command's view of the amount of memory in use
##
sub _getMemoryInUse {
  my @lines = split("\n", assertSystem('free -b')->{stdout});
  my @fields = split('\s+', $lines[0]);
  unshift(@fields, 'label');
  my @values = split('\s+', $lines[1]);
  my %free = map { ($_, shift(@values)) } @fields;
  return $free{used};
}

1;
