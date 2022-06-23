##
# Class for testing VDO Performance with the configurations we know
# FusionIO used during evaluation.
#
# See ESC-519 for details
#
# $Id$
##
package VDOTest::VDOFusionIOPerfTest;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Constants;

use base qw(VDOTest::VDOFIOPerfTest);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple don't fill the index
     albFill              => 0,
     # @ple use O_DIRECT (bypass page cache)
     directIo             => 1,
     # @ple turn this off (likely because they're graphing)
     gtod_reduce          => 0,
     # @ple iodepth parameters
     ioDepth              => 128,
     ioDepthBatchSubmit   => 16,
     # XXX Disabled due to VDO-4533 till we rebase FIO.
     #ioDepthBatchComplete => 16,
     # @ple use libaio
     ioEngine             => "libaio",
     # @ple the write pattern to use.  tests must set this. FusionIO ran write
     #  and randwrite tests.
     ioType               => undef,
     # @ple use 2 jobs
     jobCt                => 2,
     # @ple use threads instead of procs
     thread               => 1,
     # @ple don't create a fileSystem
     useFilesystem        => 0,
     # @ple don't run a size-based test
     writePerJob          => 50 * $GB,
    );
##
