##
# Tests discarding compressed data with 512 byte logical blocks.
#
# $Id$
##
package VDOTest::Discard512Compressed;
use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Constants qw($SECTOR_SIZE);

use base qw(VDOTest::Discard512);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Percentage of compressibility to use when creating data files
     compressibility       => 90,
     # @ple Chunk size in bytes in which compressible data is written
     compressibleChunkSize => $SECTOR_SIZE,
    );
##

1;
