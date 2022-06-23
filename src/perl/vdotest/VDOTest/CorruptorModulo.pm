##
# Basic functional testing of Corruptor.pm device using
# modulo corruption type.
#
# $Id$
##
package VDOTest::CorruptorModulo;

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
# Tests modulo corruption option
##
sub testModuloCorruption {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  # Write some blocks, read them back and verify the data have not changed.
  my $slice1 = $self->createSlice(blockCount => $self->{blockCount});
  $self->_writeSlice($slice1, { tag => "Direct1" });
  $self->_verifySlice($slice1);

  # Because this is based on modulo, we set the value to 8 to ensure one
  # of the 8 sectors from the block will be corrupted.
  $device->enableModuloRead(8);

  # Write the blocks again.
  my $slice2 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => $self->{blockCount},);
  $self->_writeSlice($slice2, { tag => "Direct2" });

  # The verify should fail because of reads returning bad data.
  $self->_verifySliceFailure($slice2);

  # Set the modulo value to something that won't cause corruption
  # on the second block but will on the first and verify accordingly
  # for reads.
  $device->enableModuloRead(16);
  $self->_verifySlice($slice2);
  $self->_verifySliceFailure($slice1);

  # Disable read corruption and verify again. should work
  # this time.
  $device->disableCurrentRead();
  $self->_verifySlice($slice1);

  # Restart the device to verify that good data is persistent
  $device->restart();
  $self->_verifySlice($slice1);
  $self->_verifySlice($slice2);

  # Turn on write corruption. Because this is based on modulo,
  # we set the value to 8 to ensure one of the 8 sectors from
  # the block will be corrupted.
  $device->enableModuloWrite(8);

  # Write the blocks again.
  my $slice3 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => 2 * $self->{blockCount},);
  $self->_writeSlice($slice3, { tag => "Direct3" });

  # The verify should fail because we wrote bad data.
  $self->_verifySliceFailure($slice3);

  # Disable write corruption and verify again. should still
  # fail because the data is still bad.
  $device->disableCurrentWrite();
  $self->_verifySliceFailure($slice3);

  # Have writes fail for each 12th sector. This means when we
  # write the fourth block (sectors 24-31) we shouldn't get any
  # corruption.
  $device->enableModuloWrite(11);

  # Write the blocks again.
  my $slice4 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => 3 * $self->{blockCount},);
  $self->_writeSlice($slice4, { tag => "Direct4" });
  $self->_verifySlice($slice4);

  # Restart the device to verify that good and bad data is persistant.
  $device->restart();
  $self->_verifySlice($slice4);
  $self->_verifySliceFailure($slice3);
}

1;
