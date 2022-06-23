##
# Major/minor device number testing
#
# $Id$
##
package VDOTest::MajorMinor;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple Use a VDO device
  deviceType => "linear",
);
##

########################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);

  $self->SUPER::set_up();
  $self->createTestDevice("vdo",
                          useMajorMinor   => 1,
                          setupOnCreation => 0);
}

########################################################################
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  $device->start();
  my $slice = $self->createSlice(blockCount => 100);
  $slice->write(tag => "devnum");
  $slice->verify();
  $device->restart();
  $slice->verify();
  $device->stop();
}

1;
