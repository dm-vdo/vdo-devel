##
# Stress test VDO rebuild with multiple streams of operations.
#
# This test is similar to RebuildStress02, but does not include operations that
# work only on VDO devices.  This test can run on a raw device.
#
# $Id$
##
package VDOTest::Stress02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use List::Util qw(min);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(VDOTest::StressBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $EMPTY_OPS = {
                 "Generate" => 1,
                };
my $NORM_OPS = {
                "Copy"       => 0.10,
                "Generate"   => 0.10,
                "Nothing"    => 0.03,
                "Regenerate" => 0.10,
                "Remove"     => 0.33,
                "Restart"    => 0.05,
                "Tar"        => 0.11,
                "Verify"     => 0.15,
                "VerifyAll"  => 0.03,
               };
my $FULL_OPS = {
                "Nothing" => 0.05,
                "Remove"  => 0.45,
                "Restart" => 0.05,
                "Verify"  => 0.45,
               };

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The type of stress test (true -> parallel, false -> serial)
     stressParallel => 1,
    );
##

#############################################################################
# @inherit
##
sub getTable {
  my ($self) = assertNumArgs(1, @_);

  if (scalar(@{$self->{datasets}}) < 2) {
    $log->info("Using empty table");
    return $EMPTY_OPS;
  }

  my $availableBlocks = $self->getAvailableBlocks();
  if ($availableBlocks <= $self->{blocksPerDataset}) {
    $log->info("Using full table ($availableBlocks blocks free)");
    return $FULL_OPS;
  }

  return $NORM_OPS;
}

1;
