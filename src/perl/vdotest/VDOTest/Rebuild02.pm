##
# Test VDO recovery and the correctness of REQ_FLUSH
#
# Do a VDO recovery and make sure we have not lost any device writes
# that completed before the last fsync.
#
# $Id$
##
package VDOTest::Rebuild02;

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
     # @ple Number of blocks to write
     blockCount      => 100000,
     # @ple Whether to use a filesystem
     useFilesystem   => 0,
     # @ple If non-zero, randomly discard (trim) this percentage of blocks
     randomlyDiscard => 0.1,
     # @ple If non-zero, randomly zero this percentage of blocks
     randomlyZero    => 0.1,
    );
##

#############################################################################
# Run the test with a single restart.
##
sub testOnce {
  my ($self) = assertNumArgs(1, @_);
  $self->simpleRecovery($self->{blockCount}, 0, 1);
}

#############################################################################
# Run the test with a double restart.
##
sub testTwice {
  my ($self) = assertNumArgs(1, @_);
  $self->simpleRecovery($self->{blockCount}, 0, 2);
}

1;
