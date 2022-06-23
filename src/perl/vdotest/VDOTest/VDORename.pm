##
# Tests renaming LVM managed VDO devices.
#
# $Id:
##
package VDOTest::VDORename;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertNumArgs
);
use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple Use a VDO device
  deviceType => "vdo",
);
##


#############################################################################
# Test VDO rename with unmanaged vdo devices.
##
sub testRename {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $name   = $device->getDeviceName();

  $device->renameVDO($name . "A");
  $device->getVDOStats()->logStats("New stats");
  $device->renameVDO($name);
  $device->getVDOStats()->logStats("Original stats");
}

1;
