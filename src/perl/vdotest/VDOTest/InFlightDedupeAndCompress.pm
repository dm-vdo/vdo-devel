##
# Test deduplication of multiple copies of the same block being written at the
# same time.
#
# $Id$
##
package VDOTest::InFlightDedupeAndCompress;

use strict;
use warnings FATAL => qw(all);
use Cwd;
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertGENumeric
  assertLENumeric
  assertNumArgs
);
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
  my ($self)  = assertNumArgs(1, @_);
  my $device  = $self->getDevice();
  my $machine = $device->getMachine();

  # Originally used a static dataset that repeated unique 4K blocks six times
  # in a row. Start by generating the unique blocks to repeat.
  my $dataSize = 256 * $MB;
  my $dataFile = $self->generateDataFile($dataSize);

  # Use a little Perl script to read each unique block and write six
  # consecutive copies of that block to the input file.
  my $inputFile = "$self->{scratchDir}/input";
  $machine->runSystemCmd("perl -e '\n"
                         . "local \$/ = \\" . $self->{blockSize} . ";\n"
                         . "while (<>) { print; print; print; print; print; print }' "
                         . "< $dataFile > $inputFile");

  my $size      = 6 * $dataSize;
  my $blockSize = 1 * $MB;
  my $blocks    = $size / $blockSize;
  $device->ddWrite(
                   if    => $inputFile,
                   count => $blocks,
                   bs    => $blockSize,
                   conv  => "fdatasync",
                   oflag => "direct",
                  );

  my $outputFile = "$self->{scratchDir}/output";
  $device->ddRead(
                  of    => $outputFile,
                  count => $blocks,
                  bs    => $blockSize,
                 );
  $machine->runSystemCmd("cmp $inputFile $outputFile");

  my $stats = $self->getVDOStats();
  assertGENumeric($stats->{'saving percent'}, 82);
  # If we're getting concurrent dedupe, very few index queries
  # should be needed.
  assertLENumeric($stats->{'dedupe advice valid'},
                  $stats->{'bios in write'} / 100);
}

1;
