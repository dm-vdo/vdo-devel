##
# Test deduping of data that are being overwritten.
#
# This test wants to put multiple writes in flight that are writing data
# that can dedupe against blocks that are being overwritten with new data.
# The basic idea of the test is to write block X over block Y at address A,
# and at the same time, write block Y over block Z at address B.
#
# $Id$
##
package VDOTest::Direct05;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks to write
     blockCount => 1000,
     # @ple Use a VDO device
     deviceType => "lvmvdo",
     # @ple VDO slab bit count
     slabBits   => $SLAB_BITS_SMALL,
    );
##

#############################################################################
# Write identical data to slightly offset addresses.  The intent is to
# start simultaneous writes where block A overwrites X with Y, and block B
# overwrites Y with Z.
##
sub testSameData {
  my ($self) = assertNumArgs(1, @_);

  $log->info("First write and verify");
  my $slice0 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => 0,);
  $self->_writeAndVerify($slice0);

  $log->info("Second write and verify");
  my $slice1 = $self->createSlice(blockCount => $self->{blockCount},
                                  offset     => 1,);
  $self->_writeAndVerify($slice1);

  $log->info("Third write and verify");
  $self->_writeAndVerify($slice0);
}

#############################################################################
# Write some blocks, read them back and verify the data have not changed
#
# @param slice  The device slice
##
sub _writeAndVerify {
  my ($self, $slice) = assertNumArgs(2, @_);
  $slice->write(
                tag   => "Direct5",
                fsync => 1,
               );
  $self->getDevice()->getMachine()->dropCaches();
  $slice->verify();
}

1;
