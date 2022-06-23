##
# Test VDO recovery with large datasets
#
# Write large datasets, do recoveries, and make sure that the filesystem and
# data survive intact.  Try out all the various ways that GenDataFiles can
# generate and/or copy and/or remove datasets.
#
# $Id$
##
package VDOTest::Rebuild04;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertMinMaxArgs assertNumArgs);
use Permabit::Constants;
use Permabit::GenDataFiles qw(genDataFiles);

use base qw(VDOTest::RebuildBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     deviceType        => "lvmvdo-dory",
     enableCompression => 1,
    );
##

#############################################################################
# Generate a dataset and take reasonable measures to ensure that it is written
# to storage and then do a recovery. Check that the filesystem mounts cleanly
# and verify the dataset.  Repeat with a second, compressible, dataset.
##
sub testGenerateTwo {
  my ($self) = assertNumArgs(1, @_);

  $log->info("First:  Create a dataset");
  my @datasets = ($self->_genDataSet());

  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Second:  Create another dataset, this one compressible at 60%");
  push(@datasets, $self->_genDataSet(0.6));

  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;
}

#############################################################################
# Generate a dataset, take reasonable measures to ensure that it is written to
# storage, and then do a recovery. Check that the filesystem mounts cleanly,
# and verify the dataset.  Repeat with a duplicate of the dataset.
##
sub testGenerateCopyAndRemove {
  my ($self) = assertNumArgs(1, @_);

  $log->info("First:  Create a dataset");
  my @datasets = ($self->_genDataSet());

  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Second:  Create a duplicate dataset");
  push(@datasets, $datasets[0]->generate());

  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Third: Remove the original dataset");
  shift(@datasets)->rm();

  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;
}

#############################################################################
# Generate a dataset, take reasonable measures to ensure that it is written to
# storage, and then do a recovery. Check that the filesystem mounts cleanly,
# and verify the dataset.  Repeat with a copy of the dataset.
##
sub testCopy {
  my ($self) = assertNumArgs(1, @_);

  $log->info("First:  Create a dataset");
  my @datasets = ($self->_genDataSet());

  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Second:  Create a copy dataset");
  push(@datasets, $datasets[0]->cp());

  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;
}

#############################################################################
# Generate a dataset, take reasonable measures to ensure that it is written to
# storage, and then do a recovery. Check that the filesystem mounts cleanly,
# and verify the dataset.  Repeat with a copy of the dataset made with tar.
##
sub testTar {
  my ($self) = assertNumArgs(1, @_);

  $log->info("First:  Create a dataset");
  my @datasets = ($self->_genDataSet());

  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Second:  Create a copy dataset using tar");
  push(@datasets, $datasets[0]->tar());

  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;
}

#############################################################################
# Generate a set of data files
#
# @oparam compress  Compressibility fraction
#
# @return a Permabit::GenDataFiles object
##
sub _genDataSet {
  my ($self, $compress) = assertMinMaxArgs([undef], 1, 2, @_);
  return genDataFiles(
                      compress => $compress,
                      fs       => $self->getFileSystem(),
                      fsync    => 1,
                      numBytes => 5 * $GB,
                      numFiles => 100,
                     );
}

1;
