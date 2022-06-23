##
# Test VDO recovery with large datasets and multiple recoveries.
#
# Write a large dataset, do a recovery, and make sure that the filesystem and
# data survive intact.  Do this multiple times.
#
# $Id$
##
package VDOTest::Rebuild05;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::GenDataFiles qw(genDataFiles);
use Permabit::Utils qw(getRandomElement getRandomIdx);

use base qw(VDOTest::RebuildBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# String all the GenDataFiles synchronous operations together into one grand
# test
##
sub testAll {
  my ($self) = assertNumArgs(1, @_);

  # We will end up writing 2 complete datasets plus 3 duplicate datasets,
  # so size accordingly
  $log->info("First:  Create a dataset");
  my @datasets = (genDataFiles(
                               fs       => $self->getFileSystem(),
                               fsync    => 1,
                               numBytes => 2 * $GB,
                               numFiles => 100,
                              ));
  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Second:  Create another dataset");
  push(@datasets, genDataFiles(
                               fs       => $self->getFileSystem(),
                               fsync    => 1,
                               numBytes => 2 * $GB,
                               numFiles => 100,
                              ));
  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Third:  Create a duplicate dataset");
  push(@datasets, getRandomElement(\@datasets)->generate());
  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Fourth:  Copy a dataset using cp");
  push(@datasets, getRandomElement(\@datasets)->cp());
  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Fifth:  Copy a dataset using tar");
  push(@datasets, getRandomElement(\@datasets)->tar());
  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;

  $log->info("Sixth: Remove a dataset");
  splice(@datasets, getRandomIdx(\@datasets), 1)->rm();
  $self->recoverAndRestartVDO();
  map { $_->verify() } @datasets;
}

1;
