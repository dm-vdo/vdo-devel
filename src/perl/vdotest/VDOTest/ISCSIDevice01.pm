##
# Test the ISCSI BlockDevice.
#
# $Id$
##
package VDOTest::ISCSIDevice01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEq
  assertNumArgs
);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Start with a pass-through iscsi device on top of a raw device
     deviceType => "iscsi",
     # @ple use two client machines
     numClients => 2,
    );
##

#############################################################################
# Test using an ISCSI device directly.
##
sub testISCSITarget {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  assertEq($device->getMachineName(), $self->{clientNames}[0]);

  # Write a slice to the pass-through
  my $slice = $self->createSlice(blockCount => 256);
  $slice->write(tag => "initial", direct => 1, sync => 1);
  $slice->verify();

  # Migrate to another host, verify the old slice, create a new one, and verify
  # both.
  $device->migrate($self->getUserMachine($self->{clientNames}[1]));
  assertEq($device->getMachineName(), $self->{clientNames}[1]);
  $slice->verify();
  my $slice2 = $self->createSlice(blockCount => 256, offset => 256);
  $slice2->write(tag => "final", direct => 1, sync => 1);
  $slice->verify();
  $slice2->verify();

  # Migrate back to the original host and verify both slices.
  $device->migrate($self->getUserMachine($self->{clientNames}[0]));
  assertEq($device->getMachineName(), $self->{clientNames}[0]);
  $slice->verify();
  $slice2->verify();

  # Migrate back to the second host and verify both slices.
  $device->migrate($self->getUserMachine($self->{clientNames}[1]));
  assertEq($device->getMachineName(), $self->{clientNames}[1]);
  $slice->verify();
  $slice2->verify();
}

1;
