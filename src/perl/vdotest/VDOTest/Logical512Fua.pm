##
# Tests of 512 byte writes with FUA attached.
#
# $Id$
##
package VDOTest::Logical512Fua;
use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants qw($SECTOR_SIZE);

use base qw(VDOTest::Logical512);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Type of device set up by VDOTest
     deviceType => "fua-lvmvdo",
    );
##


#############################################################################
# @inherit
##
sub checkPartialWriteStats {
  my ($self, $initialStats, $finalStats) = assertNumArgs(3, @_);
  $self->SUPER::checkPartialWriteStats($initialStats, $finalStats);
  assertEqualNumeric($finalStats->{"bios acknowledged partial write"},
                     $finalStats->{"bios acknowledged partial fua"},
                     "FUA/write bios get counted as acknowledged");
}

1;
