##
# Tests of 512 byte logical block size with compressed data.
#
# $Id$
##
package VDOTest::Logical512Compressed;
use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Constants qw($SECTOR_SIZE);

use base qw(VDOTest::Logical512);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple percentage of compressibility to use when creating data files
     compressibility       => 80,
     # @ple chunk size in bytes in which compressible data is written
     compressibleChunkSize => $SECTOR_SIZE,
    );
##

1;
