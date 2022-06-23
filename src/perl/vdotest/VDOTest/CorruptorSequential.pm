##
# Basic functional testing of Corruptor.pm device using
# sequential corruption type.
#
# $Id$
##
package VDOTest::CorruptorSequential;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(VDOTest::CorruptionBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple block count for writing data
  blockCount => 1,
);
##

#############################################################################
# Tests sequential corruption option
##
sub testSequentialCorruption {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  # Write some blocks, read them back and verify the data have not changed.
  my $slice1 = $self->createSlice(blockCount => $self->{blockCount});
  $self->_writeSlice($slice1, { tag => "Direct1" });
  $self->_verifySlice($slice1);

  # Because this is based on amt of sectors read, lets set this value to
  # be sure the next read fails.
  $device->enableSequentialRead(8);

  # Write the blocks again.
  my $slice2 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => $self->{blockCount},);
  $self->_writeSlice($slice2, { tag => "Direct2" });
  # The verify should fail because of reads returning bad data.
  $self->_verifySliceFailure($slice2);

  # disable read corruption and verify again. should work.
  $device->disableCurrentRead();
  $self->_verifySlice($slice2);

  # Restart the device to verify that the good data is persistent.
  $device->restart();
  $self->_verifySlice($slice1);
  $self->_verifySlice($slice2);

  # Turn on corruption for writes. Because this is based on sectors
  # written, we'll set this to a number that will cause the write to
  # corrupt its data.
  $device->enableSequentialWrite(8);

  # Write the blocks again.
  my $slice3 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => 2 * $self->{blockCount},);
  $self->_writeSlice($slice3, { tag => "Direct3" });
  # The verify should fail because we wrote bad data
  $self->_verifySliceFailure($slice3);

  # Disable write corruption and verify again. The verify
  # should fail because the data is still bad.
  $device->disableCurrentWrite();
  $self->_verifySliceFailure($slice3);

  # Have writes fail for each 9th sector written. This means
  # we should only get a failure on second write.
  $device->enableSequentialWrite(9);

  # Write the first block.
  my $slice4 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => 3 * $self->{blockCount},);
  $self->_writeSlice($slice4, { tag => "Direct4" });
  $self->_verifySlice($slice4);

  # Write the blocks again. This time we should get corruption.
  my $slice5 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => 4 * $self->{blockCount},);
  $self->_writeSlice($slice5, { tag => "Direct5" });
  $self->_verifySliceFailure($slice5);

  # Restart the device to verify that good and bad data is persistent
  $device->restart();
  $self->_verifySlice($slice4);
  $self->_verifySliceFailure($slice5);
}

1;
