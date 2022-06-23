##
# Test a specific slab configuration which exposed a bug in vdoAudit
# without adding it to checkin.
#
# $Id$
##
package VDOTest::AuditTool02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use base qw(VDOTest::AuditTool);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a 4G slab
     slabBits => 20,
    );
##

1;
