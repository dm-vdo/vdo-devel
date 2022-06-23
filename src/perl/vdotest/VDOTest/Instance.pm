##
# Test instance number selection while creating and tearing down
# multiple VDO devices.
#
# $Id$
##
package VDOTest::Instance;

use strict;
use warnings FATAL => qw(all);
use Data::Dumper;
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEq
  assertNumArgs
);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a small albireo index
     memorySize                => 0.25,
     # @ple Don't verbosely log at shutdown
     verboseShutdownStatistics => 0,
    );
##

#############################################################################
##
sub testMultipleInstances {
  my ($self) = assertNumArgs(1, @_);
  my $volumeGroup = $self->createVolumeGroup();
  # Split the raw device size.
  $self->{physicalSize} = undef;
  my @parameters = (lvmSize       => $volumeGroup->getFreeBytes() / 3,
                    storageDevice => $self->getDevice(),
                    volumeGroup   => $volumeGroup,
                   );
  my ($deviceA, $deviceB, $deviceC)
    = map({
           my $ld = $self->createTestDevice("linear", @parameters);
           $_ = $self->createTestDevice("vdo",
                                        deviceName    => "vdo$_",
                                        storageDevice => $ld);
          } (1..3));

  $log->info("checking instance numbers");
  assertEq("0", $deviceA->getInstance());
  assertEq("1", $deviceB->getInstance());
  assertEq("2", $deviceC->getInstance());

  # Instance numbers aren't permanent for the device; each "start"
  # uses the next available at the time.
  $deviceA->stop();
  $deviceB->stop();
  $deviceB->start();
  $deviceA->start();
  assertEq("4", $deviceA->getInstance());
  assertEq("3", $deviceB->getInstance());
  assertEq("2", $deviceC->getInstance());
  $deviceB->stop();

  # Changing characteristics of the device, implemented through
  # reloading the table entry, shouldn't change the instance number.
  my $oldSize = $deviceA->getSize();
  $deviceA->growLogical($oldSize + $MB);
  assertEq("4", $deviceA->getInstance());

  # The cycle should continue where we left off instead of being
  # advanced by the reloads.
  $deviceB->start();
  assertEq("5", $deviceB->getInstance());
}

1;
