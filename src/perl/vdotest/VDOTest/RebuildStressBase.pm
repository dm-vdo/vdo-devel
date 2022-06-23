##
# Base class for VDO rebuild stress tests.
#
# $Id$
##
package VDOTest::RebuildStressBase;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(VDOTest::RebuildBase VDOTest::StressBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

# StressBase properties override RebuildBase properties.
########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDOManager device without stripfua under it.
     #      A stripFua device is inappropriate since flushes/fuas
     #      are needed to ensure necessary data persistence.
     deviceType => "lvmvdo-linear",
    );
##

########################################################################
# @inherit
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  # Deal with the multiple base classes here
  $self->tearDownDatasets();
  VDOTest::RebuildBase::tear_down($self);
}

########################################################################
# @inherit
##
sub checkFinalStressTestState {
  my ($self) = assertNumArgs(1, @_);
  $self->recoverAndRestartVDO();
  $self->getVDODevice()->waitUntilRecoveryComplete();
  $self->SUPER::checkFinalStressTestState();
}

########################################################################
# Perform a "Reboot" operation:  Do an emergency restart.
##
sub doReboot {
  my ($self) = assertNumArgs(1, @_);
  $self->doQuiesce();
  $self->recoverAndRestartVDO();
  $self->getDevice()->getMachine()->runSystemCmd("sync");
  $self->operate({ "VerifyAll" => 0.2,
                   "Verify"    => 0.3,
                   "Reboot"    => 0.2,
                   "Nothing"   => 0.3 });
}

########################################################################
# Perform a "Recover" operation:  Wait for recovery to finish.
##
sub doRecover {
  my ($self) = assertNumArgs(1, @_);
  $self->getVDODevice()->waitUntilRecoveryComplete();
}

1;
