##
# Do a VDO recovery and make sure we have not lost any device writes.
#
# $Id$
##
package VDOTest::Rebuild01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest::RebuildBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The number of blocks to write
     blockCount            => 50000,
     # @ple Chunk size in bytes in which compressible data is written
     compressibleChunkSize => 6 * $KB,
     # @ple Compression ratio
     compressibility       => 55,
     # @ple Whether to compress
     enableCompression     => 1,
     # @ple Whether to use a filesystem
     useFilesystem         => 0,
    );
##

#############################################################################
# Write some blocks, issue a flush, and make sure data survives a crash.
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  # Write many blocks, crash, and restart once.
  $self->simpleRecovery($self->{blockCount}, 0, 1);
}

1;
