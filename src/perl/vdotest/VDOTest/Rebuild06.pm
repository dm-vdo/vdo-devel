##
# Test VDO rebuild with large datasets and multiple recoveries in multiple
# streams
#
# Write large datasets using multiple streams, do a recovery, and make sure
# the file system and data survive intact.  Do this multiple times.
#
# $Id$
##
package VDOTest::Rebuild06;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::GenDataFiles qw(genDataFiles);
use Permabit::Utils qw(getRandomElement spliceRandomElement);

use base qw(VDOTest::RebuildBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  # Stop any datasets which aren't finished yet.
  foreach my $dataset (@{$self->{datasets}}, @{$self->{removing}}) {
    $self->runTearDownStep(sub { $dataset->kill() });
  }
  $self->SUPER::tear_down();
}

#############################################################################
# In a large loop, do every combination of pairs of dataset operations,
# followed by a recovery and total data verification each time.
##
sub testPairs {
  my ($self) = assertNumArgs(1, @_);

  # Use 80% of the space, allowing for 50 datasets
  $self->{_numBytes} = 0.016 * $self->getDevice()->getSize();
  # Track the datasets that can be read
  $self->{datasets} = [];
  # Track the datasets with deletes in progress
  $self->{removing} = [];

  # Choose and start two operations, trying every pairwise combination
  my @OPS = qw(Generate Regenerate Copy Tar Remove Verify);
  foreach my $op1 (@OPS) {
    my $doOp1 = "do$op1";
    foreach my $op2 (@OPS) {
      my $doOp2 = "do$op2";
      $log->info("First operation $op1");
      my @ds1 = $self->$doOp1();
      sleep(2);
      $log->info("Second operation $op2");
      my @ds2 = $self->$doOp2();
      push(@{$self->{datasets}}, @ds1, @ds2);

      # When they finish, restart and verify all the data
      map { $_->pollUntilDone() } splice(@{$self->{removing}});
      map { $_->pollUntilDone() } @{$self->{datasets}};
      $self->recoverAndRestartVDO();
      map { $self->_limitVerifies(4); $_->verify(async => 1) }
          @{$self->{datasets}};
      map { $_->pollUntilDone() } @{$self->{datasets}};
    }
  }
}

#############################################################################
# Perform a "Copy" operation:  Copy a dataset using cp
##
sub doCopy {
  my ($self) = assertNumArgs(1, @_);
  return (getRandomElement($self->{datasets})->cp(async => 1));
}

#############################################################################
# Perform a "Generate" operation:  Generate a brand new dataset
##
sub doGenerate {
  my ($self) = assertNumArgs(1, @_);
  return (genDataFiles(
                       async    => 1,
                       fs       => $self->getFileSystem(),
                       fsync    => 1,
                       numBytes => $self->{_numBytes},
                       numFiles => 100,
                      ));
}

#############################################################################
# Perform a "Regenerate" operation:  Generate a duplicate dataset
##
sub doRegenerate {
  my ($self) = assertNumArgs(1, @_);
  return (getRandomElement($self->{datasets})->generate(async => 1));
}

#############################################################################
# Perform a "Remove" operation:  Remove a dataset
##
sub doRemove {
  my ($self) = assertNumArgs(1, @_);
  my @dsList;
  for (;;) {
    my $ds = spliceRandomElement($self->{datasets});
    if ($ds->canDelete()) {
      push(@{$self->{removing}}, $ds);
      $ds->rm(async => 1);
      last;
    } else {
      push(@dsList, $ds);
    }
  }
  return @dsList;
}

#############################################################################
# Perform a "Tar" operation:  Copy a dataset using tar
##
sub doTar {
  my ($self) = assertNumArgs(1, @_);
  return (getRandomElement($self->{datasets})->tar(async => 1));
}

#############################################################################
# Perform a "Verify" operation:  Verify a dataset
##
sub doVerify {
  my ($self) = assertNumArgs(1, @_);
  getRandomElement($self->{datasets})->verify(async => 1);
  return ();
}

#############################################################################
# Limit the number of parallel verify tasks that we run.  Wait while there
# are more than N tasks in progress
#
# @param limit  The maximum number of tasks to execute in parallel
##
sub _limitVerifies {
  my ($self, $limit) = assertNumArgs(2, @_);
  local $_;
  # A dataset that is being verified cannot be deleted, so we count the
  # number of datasets that cannot be deleted.
  while (scalar(grep { !$_->canDelete() } @{$self->{datasets}}) >= $limit) {
    map { $_->poll() } @{$self->{datasets}};
    sleep(1);
  }
}

1;
