##
# Basic VDO functional test
#
# $Id$
##
package VDOTest::InFlightDedupeAndCompress;

use strict;
use warnings FATAL => qw(all);
use Cwd;
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertEq
                            assertEqualNumeric
                            assertGENumeric
                            assertLENumeric
                            assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %PROPERTIES
  = (
     # @ple Use a VDO device
     deviceType        => "lvmvdo",
     # @ple Enable compression on the VDO device
     enableCompression => 1,
    );

######################################################################
# Test that large numbers of blocks with the same data are written
# correctly (VDO-2711).
##
sub testInFlightDedupeWithCompression {
  my ($self)    = assertNumArgs(1, @_);
  my $device    = $self->getDevice();
  my $machine   = $device->getMachine();
  my $inputFile = '/permabit/datasets/vdo/iobw.tst';
  my $size      = 2 * $GB;
  my $blockSize = 1 * $MB;
  my $blocks    = $size / $blockSize;
  $device->ddWrite(
                   if    => $inputFile,
                   count => $blocks,
                   bs    => $blockSize,
                   conv  => "fdatasync",
                   oflag => "direct",
                  );

  my $readBack = "$self->{scratchDir}/iobw.tst";
  $device->ddRead(
                  of    => $readBack,
                  count => $blocks,
                  bs    => $blockSize,
                 );
  $machine->runSystemCmd("cmp $inputFile $readBack");

  my $stats = $self->getVDOStats();
  assertGENumeric($stats->{'saving percent'}, 82);
  # If we're getting concurrent dedupe, very few index queries
  # should be needed.
  assertLENumeric($stats->{'dedupe advice valid'},
                  $stats->{'bios in write'} / 100);
}

1;
