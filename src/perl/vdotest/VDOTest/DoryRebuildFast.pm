##
# Do a very fast rebuild test with Dory.
#
# $Id$
##
package VDOTest::DoryRebuildFast;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use base qw(VDOTest::DoryRebuild01);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Compression settings to try
     compressFractions => [ 0 ],
     # @ple Dedupe settings to try
     dedupeFractions   => [ 0.1 ],
     # @ple Time to write each dataset before a rebuild, in seconds
     timePerSlice      => 20,
    );
##

1;
