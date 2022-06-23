##
# Verify that VDO can be upgraded from last branch with multiple large slabs
# full of data.
#
# $Id$
##
package VDOTest::LargeUpgrade;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Constants;

use base qw(VDOTest::UpgradeLatest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple Number of blocks to write
   blockCount   => 12000000,
   # @ple VDO logical size
   logicalSize  => 1 * $TB,
   # @ple VDO physical size
   physicalSize => 100 * $GB,
   # @ple VDO slab bit count
   slabBits     => $SLAB_BITS_LARGE,
  );
##

1;
