##
# Test that specific sequences of block devices are usable.
#
# $Id$
##
package VDOTest::BlockDevice02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# This test is inspired by VDO-2976.  The failure is reproduced by running any
# test that uses a managed device (on a default device), and following that
# with a test that wants to put a linear volume on the same default device.
# This sequence fails on RHEL7, because RHEL7 works hard to demand manual
# intervention to verify that this sequence of operations is allowed.
#
# We repeat that sequence here, and expect the Permabit::VolumeGroup
# implementation to work around the difficulty.
##
sub testManagedFollowedByLinear {
  my ($self) = assertNumArgs(1, @_);
  my $rawDevice = $self->getDevice();

  # First put a managed device on the raw device.
  my $vdoDevice = $self->createTestDevice("lvmvdo",
                                          storageDevice => $rawDevice);

  # Then tear it down and smush it.
  $self->destroyTestDevice($vdoDevice);

  # Now put a linear device on the raw device.
  my $linearDevice = $self->createTestDevice("linear",
                                             lvmSize       => 5 * $GB,
                                             storageDevice => $rawDevice);

  # And put a filesystem on the linear device.
  $self->createFileSystem($linearDevice);
}

1;
