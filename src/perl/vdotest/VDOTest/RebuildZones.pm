##
# Do a VDO recovery and confirm that the rebuild works with a
# different number of physical zones.
#
# $Id$
##
package VDOTest::RebuildZones;

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
     # @ple How many blocks of data to write
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
# Go from n to n+1 physical zones during restart, then back during a restart.
##
sub testZoneChange {
  my ($self) = assertNumArgs(1, @_);
  $self->simpleRecovery($self->{blockCount}, 1, 1);
  $self->simpleRecovery($self->{blockCount}, -1, 1);
}

1;
