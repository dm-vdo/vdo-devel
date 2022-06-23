##
# Stress test a block device with multiple streams of operations.
#
# This test is similar to RebuildStress03, but does not include operations that
# work only on VDO devices.  This test can run on a raw device.
#
# $Id$
##
package VDOTest::Stress01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(VDOTest::StressBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $EMPTY_OPS = {
                 "Generate" => 1,
                };
my $NORM_OPS = {
                "Copy"       => 0.09,
                "Generate"   => 0.09,
                "Nothing"    => 0.06,
                "Regenerate" => 0.09,
                "Remove"     => 0.30,
                "Restart"    => 0.02,
                "Sleep"      => 0.10,
                "Tar"        => 0.09,
                "Verify"     => 0.15,
                "VerifyAll"  => 0.01,
               };
my $FULL_OPS = {
                "Nothing" => 0.07,
                "Quiesce" => 0.05,
                "Remove"  => 0.38,
                "Restart" => 0.05,
                "Verify"  => 0.45,
               };

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Enable compression
     enableCompression => 1,
     # @ple The type of stress test (true -> parallel, false -> serial)
     stressParallel    => 1,
    );
##

#############################################################################
# @inherit
##
sub getTable {
  my ($self) = assertNumArgs(1, @_);

  if (scalar(@{$self->{datasets}}) < 2) {
    $log->info("Using empty operations");
    return $EMPTY_OPS;
  }

  my $availableBlocks = $self->getAvailableBlocks();
  if ($availableBlocks <= $self->{blocksPerDataset}) {
    $log->info("Using full table ($availableBlocks blocks free)");
    return $FULL_OPS;
  }

  $log->info("Using normal operations");
  return $NORM_OPS;
}

1;
