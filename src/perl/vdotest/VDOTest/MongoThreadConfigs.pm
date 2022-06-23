##
# Find the best thread config for VDO.
#
# $Id$
##
package VDOTest::MongoThreadConfigs;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Constants;

use base qw(VDOTest::Mongo);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES =
  (
   # @ple Don't run very many iterations, most are pretty obvious.
   iterationCount       => 1,
   # @ple VDO logical size
   logicalSize          => 15 * $GB,
   # @ple VDO physical size
   physicalSize         => 12 * $GB,
   # @ple Don't prewrite data -- top performance is on empty VDO.
   preWriteData         => 0,
   # @ple Reformat between each test.
   reformatEachTest     => 1,
   # @ple VDO slab bit count
   slabBits             => $SLAB_BITS_TINY,
   # @ple Test all of the reasonable thread configs on a reasonable
   #      workload, max 1G / 30 seconds apiece.
   testVectors          => {
			    bioAckThreadCount   => [1, 2, 4],
			    bioThreadCount      => [2, 4, 6, 8],
			    cpuThreadCount      => [1 .. 4],
			    hashZoneThreadCount => [1 .. 2],
			    logicalThreadCount  => [1 .. 6],
			    physicalThreadCount => [1 .. 4],
		            dedupePercent       => [50],
			    compressPercent     => [55],
                            ioDepth             => [1024],
                            ioType              => ["randwrite"],
                            jobCt               => [4],
			    runTime		=> [30],
			    writePerJob         => [$GB],
			    offsetIncrement     => [$GB],
                           },
  );
##

1;
