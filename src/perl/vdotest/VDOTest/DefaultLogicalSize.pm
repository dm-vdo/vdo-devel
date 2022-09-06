##
# Check that LVM can create a VDO with the default logical size.
#
# $Id$
##
package VDOTest::DefaultLogicalSize;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEq
  assertNumArgs
  assertTrue
);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDO device
     deviceType    => "lvmvdo",
     # @ple Ensure the default logical size
     logicalSize   => 0,
    );
##

#############################################################################
##
sub testDefaultLogicalSize {
  my ($self) = assertNumArgs(1, @_);
}

1;
