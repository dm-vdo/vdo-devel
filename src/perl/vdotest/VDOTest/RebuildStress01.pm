##
# Stress test VDO rebuild with a single stream of operations.
#
# $Id$
##
package VDOTest::RebuildStress01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(VDOTest::RebuildStressBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $EMPTY_OPS = {
                 "Generate" => 1,
                };
my $NORM_OPS = {
                "Copy"       => 0.11,
                "Generate"   => 0.10,
                "Reboot"     => 0.01,
                "Recover"    => 0.03,
                "Regenerate" => 0.11,
                "Remove"     => 0.30,
                "Tar"        => 0.11,
                "Verify"     => 0.20,
                "VerifyAll"  => 0.03,
               };
my $FULL_OPS = {
                "Reboot"  => 0.05,
                "Recover" => 0.10,
                "Remove"  => 0.85,
               };

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

#############################################################################
# @inherit
##
sub doCopy {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::doCopy();
  $self->operate({ "Reboot" => 0.5, "Nothing" => 0.5 });
}

#############################################################################
# @inherit
##
sub doGenerate {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::doGenerate();
  $self->operate({ "Reboot" => 0.5, "Nothing" => 0.5 });
}

#############################################################################
# @inherit
##
sub doRegenerate {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::doRegenerate();
  $self->operate({ "Reboot" => 0.5, "Nothing" => 0.5 });
}

#############################################################################
# @inherit
##
sub doTar {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::doTar();
  $self->operate({ "Reboot" => 0.5, "Nothing" => 0.5 });
}

1;
