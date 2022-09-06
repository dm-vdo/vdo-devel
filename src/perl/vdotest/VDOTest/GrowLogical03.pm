##
# Test that VDO will auto-grow if the logical size in the initial load
# is larger than the value in the super block.
#
# $Id$
##
package VDOTest::GrowLogical03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEqualNumeric
  assertNumArgs
);
use Permabit::Constants;
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDO device
     deviceType    => "vdo",
     # @ple Initial logical size
     logicalSize   => 5 * $GB,
    );
##

#############################################################################
# Stop the device, and then start it again with a larger table to force an
# auto grow-logical.
#
# @return A slice which can be used to test that the growth succeeded
##
sub resize {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  $device->stop();
  $device->{logicalSize} *= 2;
  $device->start();

  my $logicalBlocks = int($device->{logicalSize} / $self->{blockSize});
  assertEqualNumeric($device->getVDOStats()->{"logical blocks"},
                     $logicalBlocks,
                     "logical blocks should reflect auto-grow operation");
  return $self->createSlice(blockCount => 20,
                            offset => $logicalBlocks - 21);
}

#############################################################################
# Start the VDO with a larger size than it was formatted for, and show that
# we can write to it.
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  my $slice = $self->resize();
  $slice->write(tag => 'basic', fsync => 1);
  $slice->verify();
  $self->getDevice()->restart();
  $slice->verify();

  my $slice2 = $self->resize();
  $slice2->write(tag => 'basic2', fsync => 1);
  $slice->verify();
  $slice2->verify();

  $self->getDevice()->restart();
  $slice->verify();
  $slice2->verify();
}

1;
