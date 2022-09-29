##
# Test that requesting a VDO device with a full index succeeds.
#
# The tests that want to do use this option are all performance tests.  We just
# want to make sure that this part of the infrastructure does the right thing.
#
# $Id$
##
package VDOTest::BlockDevice03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertGENumeric assertGTNumeric assertNumArgs);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple fill index first
     albFill    => 1,
     # @ple use a managed vdo device
     deviceType => "lvmvdo",
    );
##

########################################################################
# Get the capacity of the index
##
sub getIndexCapacity {
  my ($self) = assertNumArgs(1, @_);

  my $memorySize = $self->{memorySize};
  my $chaptersPerVolume = 1024;
  my $recordsPerChapter;

  if ($memorySize eq "0.25") {
    $recordsPerChapter = 65536;
  } elsif ($memorySize eq "0.5" or $memorySize eq "0.50") {
    $recordsPerChapter = 131072;
  } elsif ($memorySize eq "0.75") {
    $recordsPerChapter = 196608;
  } else {
    $recordsPerChapter = 262144;
    $chaptersPerVolume *= $memorySize;
  }

  my $recordsPerVolume = $recordsPerChapter * $chaptersPerVolume;
  if ($self->{sparse}) {
    $recordsPerVolume *= 10;
  }

  return ($recordsPerChapter, $recordsPerVolume);
}

########################################################################
##
sub testIndexPreFull {
  my ($self) = assertNumArgs(1, @_);

  my ($recordsPerChapter, $recordsPerVolume) = $self->getIndexCapacity();
  my $stats;
  # Filling an empty dense index must have added at least a full
  # volume of records.
  if (!$self->{sparse}) {
    $stats = $self->getDevice()->getVDOStats();
    assertGENumeric($stats->{"entries indexed"}, $recordsPerVolume);
  }
  # Writing another chapter should cause entries to be discarded
  my $slice = $self->createSlice(blockCount => $recordsPerChapter);
  $slice->write(tag => "Fill", dedupe => 0.0, fsync => 1);
  $stats = $self->getDevice()->getVDOStats();
  assertGTNumeric($stats->{"entries discarded"}, 0);
}

1;
