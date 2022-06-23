##
# Basic functional testing of Corruptor.pm device using
# random corruption type.
#
# $Id$
##
package VDOTest::CorruptorRandom;

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
# Tests random corruption option
##
sub testRandomCorruption {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  # Write some blocks, read them back and verify the data have not changed.
  my $slice1 = $self->createSlice(blockCount => $self->{blockCount});
  $self->_writeSlice($slice1, { tag => "Direct1" });
  $self->_verifySlice($slice1);

  # Because this is based on random numbers, theres only one value
  # that can guarentee corruption.
  $device->enableRandomRead(1);

  # Write the blocks again.
  my $slice2 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => $self->{blockCount},);
  $self->_writeSlice($slice2, { tag => "Direct2" });

  # The verify should fail because of reads returning bad data.
  $self->_verifySliceFailure($slice2);

  # disable read corruption and verify again. should work
  # this time.
  $device->disableCurrentRead();
  $self->_verifySlice($slice2);

  # Restart the device to verify that the good data is persistent.
  $device->restart();
  $self->_verifySlice($slice1);
  $self->_verifySlice($slice2);

  # Turn on corruption for writes. Because this is based on random
  # numbers, theres only one value that can guarentee corruption.
  $device->enableRandomWrite(1);

  # Write the blocks again.
  my $slice3 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => 2 * $self->{blockCount},);
  $self->_writeSlice($slice3, { tag => "Direct3" });

  # The verify should fail because we wrote bad data
  $self->_verifySliceFailure($slice3);

  # disable write corruption and verify again. The verify
  # should fail because the data is still bad.
  $device->disableCurrentWrite();
  $self->_verifySliceFailure($slice3);

  # Restart the device to verify that bad data is persistent
  $device->restart();
  $self->_verifySliceFailure($slice3);
}

1;
