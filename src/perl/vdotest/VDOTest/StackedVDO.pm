##
# Test that VDO devices can be stacked without triggering weird warnings
# intended for other situations.
#
# $Id$
##
package VDOTest::StackedVDO;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Basename;
use Log::Log4perl;
use Permabit::Assertions qw(assertEq assertFalse assertNumArgs);
use Permabit::Constants;
use Permabit::Utils qw(reallySleep retryUntilTimeout);
use Permabit::VDOTask::DDWriteTask;
use Permabit::VDOTask::FIOTask;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple A second managed VDO device atop the "main" one
     _device2     => undef,
     # @ple what class of machine to run the test on
     clientClass  => "ALBIREO-PMI",
     # @ple Use a managed VDO device
     deviceType   => "lvmvdo",
     # @ple Don't complain about slow I/O, it's part of the test
     latencyLimit => 90,
     # @ple Use a larger than normal logical space for the lower VDO device
     logicalSize  => "30G",
    );
##

# The message we look for
my $msg1 = "kvdoMapBio called from within a VDO thread";
my $msg2 = "queued an I/O request to avoid deadlock";

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();
  $self->{_device2} = $self->createTestDevice($self->{deviceType},
                                              deviceName  => 'otherVDO',
                                              logicalSize => 10 * $MB,
                                              slabBits    => $SLAB_BITS_TINY,
                                              stackable   => 1);
}

#############################################################################
# Test that the workaround code path for handling I/O submissions from a VDO
# thread is NOT followed when two different VDO devices are used.
#
# The original workaround to avoid a deadlock in the VDO driver is to avoid
# blocking *any* VDO thread when it submits I/O operations to *any* VDO
# device. That would affect cases when one VDO device is stacked atop another,
# which would just be silly and shouldn't happen at customer sites. But it
# still should work okay and not trigger the workaround warnings. The newer
# workaround should be able to ignore these cases.
##
sub testNoLoopbackDeadlockAvoidance {
  my ($self) = assertNumArgs(1, @_);

  my $vdo          = $self->{_device2};
  my $machine      = $vdo->getMachine();
  my $kernelCursor = $machine->getKernelJournalCursor();

  # Options to write a little bit to a VDO device.
  #
  # Even writing one block should be enough. In addition to the data block,
  # there'll be at least one journal block written.
  my %ddParams = (
                  bs    => 4096,
                  count => 3,
                  conv  => "fsync",
                  if    => "/dev/urandom",
                  oflag => "direct",
                 );

  # First, test that VDO lets the admin know if we detect a VDO device trying
  # to do I/O to a VDO device, using one stacked atop another.
  $vdo->ddWrite(%ddParams);
  reallySleep(60);
  # We should not get log messages about the bad cases the driver is really
  # looking for.
  assertFalse($machine->searchKernelJournalSince($kernelCursor, $msg1));
  assertFalse($machine->searchKernelJournalSince($kernelCursor, $msg2));
}

1;
