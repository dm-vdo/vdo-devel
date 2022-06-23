##
# Test the migration of devices above the ISCSI BlockDevice.
#
# $Id$
##
package VDOTest::ISCSIDevice02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use base qw(VDOTest::ISCSIDevice01);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a linear device under ISCSI to allow resizing
     deviceType => "lvmvdo-iscsi-linear",
    );
##

1;
