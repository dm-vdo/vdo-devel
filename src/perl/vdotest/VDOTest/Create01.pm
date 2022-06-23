##
# Basic VDOTest infrastructure test that sets up a VDO device
#
# $Id$
##
package VDOTest::Create01;

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
  deviceType => "lvmvdo",
);
##

#############################################################################
##
sub testCreate01 {
  my ($self) = assertNumArgs(1, @_);
}

1;
