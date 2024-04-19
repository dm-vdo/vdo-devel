##
# Test the vdoRegenerateGeometry tool.
#
# $Id$
##
package VDOTest::RegenerateGeometry;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEqualNumeric
  assertMinArgs
  assertMinMaxArgs
  assertNumArgs
);
use Permabit::Constants;
use Permabit::SystemUtils qw(
  checkResult
  runCommand
);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple Use a VDO device
  deviceType => "lvmvdo",
);
##

########################################################################
# @inherit
##
sub listSharedFiles {
  my ($self) = assertNumArgs(1, @_);
  return ($self->SUPER::listSharedFiles(),
          "src/c++/vdo/user/vdoRegenerateGeometry");
}

########################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();

  # Now that we've set up the stack, destroy the VDO and wipe out any
  # super blocks other than the one we expect.
  my $device = $self->getDevice();
  $device->stop();
  $device->enableWritableStorage();

  my @offsets;
  my $result = $self->regenerate();
  if ($result->{returnValue} == 0) {
    if ($result->{stdout} =~ /at block (\d+)/) {
      @offsets = ($1 * $PHYSICAL_BLOCK_SIZE);
    } else {
      $device->disableWritableStorage();
      die("regenerate succeeded with no candidate");
    }
  } else {
    @offsets = map { /^offset: (\d+)/ } split("\n", $result->{stdout});
  }

  $self->overwriteBlocks(@offsets);

  $device->disableWritableStorage();
}

########################################################################
# Test regenerating the geometry with up to four candidates, 2 dense, 2
# sparse.
##
sub testRegenerateGeometry {
  my ($self) = assertNumArgs(1, @_);
  my @offsets;
  for (my $config = 0; $config < 4; $config++) {
    $self->destroyTestDevice($self->getDevice());
    @offsets = $self->regenerateTest((($config % 2) == 1) ? '0.5' : '0.25',
                                     ($config > 1),
                                     $config + 1);
  }

  # Now wipe out all the candidates and test with none.
  $self->overwriteGeometry();
  $self->overwriteBlocks(@offsets);

  my $result = $self->regenerate();
  $self->getDevice()->disableWritableStorage();
  if ($result->{returnValue} == 0) {
    die("regeneration unexpectedly succeeded");
  }

}

########################################################################
# Test regeneration of a given memory config.
#
# @param memory              The index memory for the VDO
# @param sparse              Whether or not to make the index sparse
# @param expectedCandidates  The expected number of super block candidates
##
sub regenerateTest {
  my ($self, $memory, $sparse, $expectedCandidates) = assertNumArgs(4, @_);

  $log->info("testing regenerate of $memory"
             . ($sparse ? ', sparse ' : ' ')
             . "with $expectedCandidates expected candidates");
  $self->createTestDevice($self->{deviceType},
                          memorySize => $memory,
                          sparse => $sparse);

  my $slice = $self->createSlice(blockCount => 100);
  $slice->write(tag => "$expectedCandidates", dedupe => 0.5, direct => 1);
  $slice->verify();

  $self->overwriteGeometry();
  my $result = $self->regenerate();

  my @offsets = ();
  if ($expectedCandidates == 1) {
    checkResult($result);
  } else {
    if ($result->{returnValue} == 0) {
      $self->getDevice()->disableWritableStorage();
      die("regeneration unexpectedly succeeded");
    }

    my $offset;
    foreach my $line (split("\n", $result->{stdout})) {
      if ($line !~ /offset: (\d+), index memory ([\d\.]+)/) {
        next;
      }

      push(@offsets, $1);
      my $candidateMemory = $2;

      if ($candidateMemory ne $memory) {
        next;
      }

      if ($line =~ /sparse/) {
        if (!$sparse) {
          next;
        }
      } elsif ($sparse) {
        next;
      }

      $offset = $offsets[-1];
    }

    my $candidates = scalar(@offsets);
    assertEqualNumeric($candidates, $expectedCandidates,
                       "found $candidates candidate super blocks, not the "
                       . "$expectedCandidates expected");
    if (!defined($offset)) {
      $self->getDevice()->disableWritableStorage();
      die("Failed to find offset for $memory"
          . ($sparse ? ', sparse ' : ' ') . 'config');
    }

    $self->regenerate($offset);
  }

  my $device = $self->getDevice();
  $device->disableWritableStorage();
  $device->start();
  $slice->verify();

  return @offsets;
}

########################################################################
# Stop the VDO device, smash its geometry block, and confirm that the geometry
# was smashed. Leaves the device stopped and the backing store writable.
##
sub overwriteGeometry {
  my ($self) = assertNumArgs(1, @_);

  # Wipe out the geometry.
  my $device = $self->getDevice();
  $device->stop();
  $device->enableWritableStorage();
  $self->overwriteBlocks(0);
  $device->disableWritableStorage();

  # Confirm that we've done it.
  eval {
    $device->start();
  };
  if (!$EVAL_ERROR) {
    die("Device unexpectedly started");
  }

  $device->enableWritableStorage();
}

########################################################################
# Overwrite blocks with random data.
#
# @param offsets  A list of offsets of the blocks to overwrite
##
sub overwriteBlocks {
  my ($self, @offsets) = assertMinArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  foreach my $offset (@offsets) {
    $machine->dd(
                 bs    => $PHYSICAL_BLOCK_SIZE,
                 conv  => 'fsync',
                 count => 1,
                 seek  => $offset / $PHYSICAL_BLOCK_SIZE,
                 if    => '/dev/urandom',
                 of    => $device->getVDOStoragePath(),
                 oflag => 'direct'
                );
  }
}

########################################################################
# Run the regenerate command on the backing device. It must already
# have been made writable.
#
# @oparam offset  The offset to pass to the regenerate command
##
sub regenerate {
  my ($self, $offset) = assertMinMaxArgs(1, 2, @_);

  $self->{_regenerate} //= $self->findBinary('vdoRegenerateGeometry');
  my @command = ("sudo $self->{_regenerate}");
  if (defined($offset)) {
    push(@command, "--offset $offset");
  }

  my $device = $self->getDevice();
  push(@command, $device->getVDOStoragePath());
  return runCommand($device->getMachineName(), [@command]);
}

1;
