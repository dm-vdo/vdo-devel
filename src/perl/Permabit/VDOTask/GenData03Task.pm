##
# This is a custom AsyncTask for VDOTest::GenData03 (q.v.)
#
# $Id$
##
package Permabit::VDOTask::GenData03Task;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Assertions qw(assertGTNumeric assertNumArgs);
use Permabit::GenDataFiles qw(genDataFiles);

use base qw(Permabit::VDOTask);

########################################################################
# Set up a new Permabit::VDOTask::GenData03Task.
#
# @param taskNumber  The task number.
# @param numTasks    The number of tasks.
# @param fs          The filesystem.
# @param dataSize    The size of a dataset.
# @param blockSize   The block size.
#
# @return the new Permabit::VDOTask::GenData03Task.
##
sub new {
  my ($invocant, $taskNumber, $numTasks, $fs, $dataSize, $blockSize)
    = assertNumArgs(6, @_);
  my $self = $invocant->SUPER::new();
  $self->{taskNumber} = $taskNumber;
  $self->{numTasks}   = $numTasks;
  $self->{fs}         = $fs;
  $self->{dataSize}   = $dataSize;
  $self->{blockSize}  = $blockSize;
  $self->useFileSystem($fs);
  return $self;
}

########################################################################
# @inherit
##
sub taskCode {
  my ($self) = assertNumArgs(1, @_);

  # Generate datasets for iteration 1.  The size of these sets is defined to
  # stagger execution of all the tasks.
  my $firstSize
    = $self->{dataSize} * (1 + $self->{taskNumber} / $self->{numTasks}) / 2;
  my @datasets = $self->_genDataSets(1, $firstSize);

  foreach my $round (2 .. 10) {
    # Generate datasets for iteration N
    my @newDatasets = $self->_genDataSets($round, $self->{dataSize});
    # Verify and delete the previous datasets
    map { $_->verify() } @datasets;
    map { $_->rm() } @datasets;
    @datasets = @newDatasets;
  }

  # Verify and delete the last datasets
  map { $_->verify() } @datasets;
  map { $_->rm() } @datasets;
}

########################################################################
# Write datasets to the filesystem
#
# @param iteration  Iteration number
# @param dataSize   Size of data set
#
# @return  a list of Permabit::GenDataFiles
##
sub _genDataSets {
  my ($self, $iteration, $dataSize) = assertNumArgs(3, @_);

  # Divide the data space into equal sized datasets with differing numbers of
  # files.
  my @tags = (qw(H L M S T));
  my @numFiles = (64, 512, 4096, 32768);
  while ($numFiles[-1] * $self->{blockSize} * scalar(@numFiles) < $dataSize) {
    # The file size in the dataset with the largest number of files is smaller
    # than 1 block, so reduce the number of datasets.
    pop(@numFiles);
    assertGTNumeric(0, scalar(@numFiles));
  }
  my $numBytes = $dataSize / scalar(@numFiles);

  # Write the datasets
  my @data;
  while (scalar(@numFiles) > 0) {
    my $tag = $self->{taskNumber} . shift(@tags) . $iteration;
    push(@data, genDataFiles(
                             dedupe   => 0.5,
                             fs       => $self->{fs},
                             numBytes => $numBytes,
                             numFiles => shift(@numFiles),
                             tag      => $tag,
                            ));
  }
  return @data;
}

1;
