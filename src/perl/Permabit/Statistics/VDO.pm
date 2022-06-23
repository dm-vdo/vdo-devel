##
# Perl object that represents a set of VDO statistics
#
# $Id$
##
package Permabit::Statistics::VDO;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertEqualNumeric
  assertMinArgs
  assertMinMaxArgs
  assertNumArgs
);
use Permabit::Utils qw(arrayDifference);
use Permabit::Statistics::Definitions qw($VDO_STATISTICS_DEFINITIONS);
use Permabit::Statistics::Utils qw(
  getStatisticsMaps
  prepareStatisticsProperties
);

use base qw(Permabit::Statistics);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

######################################################################
# Verify that the statistics data matches the definition version
##
sub checkStatisticsVersion {
  my ($self) = assertNumArgs(1, @_);
  # force numerical comparison
  my $requiredVersion = $self->{definitions}{StatisticsVersion} + 0;
  my $version         = $self->{version} + 0;
  assertEqualNumeric($requiredVersion, $version,
		     "statistics version $version should match " .
		     "$requiredVersion");

  $requiredVersion = $self->{definitions}{ReleaseVersion} + 0;
  $version = $self->{'release version'} + 0;
  assertEqualNumeric($requiredVersion, $version,
		     "statistics release version $version should match " .
		     "$requiredVersion");
}

# Extract field definitions from the auto-generated Definitions.pm.
my %PROPERTIES = prepareStatisticsProperties($VDO_STATISTICS_DEFINITIONS,
                                             'VDOStats');

#############################################################################
# Creates a C<Permabit::Statistics::VDO>. C<new> optionally takes arguments, in
# the form of key-value pairs.
#
# @return a new C<Permabit::Statistics::VDO>
##
sub new {
  my $invocant = shift(@_);
  my $self     = $invocant->SUPER::new(%PROPERTIES,
                                       # Overrides previous values
                                       @_);

  $self->checkStatisticsVersion();
  return $self;
}

#############################################################################
# Get the number of physical blocks that are free.
#
# @return the number of free blocks
##
sub getFreeBlocks {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{"operating mode"} eq "recovering") {
    # In recovery mode, free blocks cannot be known.
    return -1;
  }

  return ($self->{"physical blocks"}
          - $self->{"data blocks used"}
          - $self->{"overhead blocks used"});
}

#############################################################################
# Get the number of data write bios on disk.
#
# @return the number of data writes on disk
##
sub getWritesOnDisk {
  my ($self) = assertNumArgs(1, @_);
  return $self->{"bios out write"} - $self->{"bios out completed write"};
}

#############################################################################
# Get the number of data read bios on disk.
#
# @return the number of data reads on disk
##
sub getReadsOnDisk {
  my ($self) = assertNumArgs(1, @_);
  return $self->{"bios out read"} - $self->{"bios out completed read"};
}

#############################################################################
# Get the number of page cache write bios on disk.
#
# @return the number of page cache writes on disk
##
sub getPageCacheWritesOnDisk {
  my ($self) = assertNumArgs(1, @_);
  return $self->{"bios page cache write"}
    - $self->{"bios page cache completed write"};
}

#############################################################################
# Get the number of page cache read bios on disk.
#
# @return the number of page cache reads on disk
##
sub getPageCacheReadsOnDisk {
  my ($self) = assertNumArgs(1, @_);
  return $self->{"bios page cache read"}
    - $self->{"bios page cache completed read"};
}

#############################################################################
# Get the number of journal write bios on disk.
#
# @return the number of journal writes on disk
##
sub getJournalWritesOnDisk {
  my ($self) = assertNumArgs(1, @_);
  return $self->{"bios journal write"}
    - $self->{"bios journal completed write"};
}

#############################################################################
# Get the number of journal read bios on disk.
#
# @return the number of journal reads on disk
##
sub getJournalReadsOnDisk {
  my ($self) = assertNumArgs(1, @_);
  return $self->{"bios journal read"}
    - $self->{"bios journal completed read"};
}

#############################################################################
# Get the total number of bios on disk.
#
# @return the number of page cache reads on disk
##
sub getIOsOnDisk {
  my ($self) = assertNumArgs(1, @_);
  return $self->getWritesOnDisk() + $self->getReadsOnDisk()
    + $self->getPageCacheWritesOnDisk() + $self->getPageCacheReadsOnDisk()
    + $self->getJournalWritesOnDisk() + $self->getJournalReadsOnDisk();
}

#############################################################################
# @inherit
##
sub logStats {
  my ($self, $label, $prefix) = assertMinMaxArgs([""], 2, 3, @_);
  $self->SUPER::logStats($label, $prefix);
  foreach my $key (sort(keys(%{$self->{typeMap}}))) {
    if (defined($self->{$key}) && ($self->{typeMap}->{$key} ne "hash")) {
      $log->info("$prefix  $key => $self->{$key}");
    }
  }
}

#############################################################################
# Log some derived statistics
#
# @param  label   Label logged with the statistics
# @oparam prefix  Prefix string used to indent the log messages
##
sub logDerivedStats {
  my ($self, $label, $prefix) = assertMinMaxArgs([""], 2, 3, @_);
  $log->info("$prefix$label");
  if ($self->{"bios in write"} > 0) {
    $log->info(sprintf("$prefix total metadata writes per data write  => \%.4f",
                       ((1.0 * $self->{"bios meta write"})
                        / $self->{"bios in write"})));
    $log->info(sprintf("$prefix journal writes per data write         => \%.4f",
                       ((1.0 * $self->{"journal blocks committed"})
                        / $self->{"bios in write"})));
    $log->info(sprintf("$prefix slab journal writes per data write    => \%.4f",
                       ((1.0 * $self->{"slab journal blocks written"})
                        / $self->{"bios in write"})));
    $log->info(sprintf("$prefix slab summary writes per data write    => \%.4f",
                       ((1.0 * $self->{"slab summary blocks written"})
                        / $self->{"bios in write"})));
    $log->info(sprintf("$prefix reference block writes per data write => \%.4f",
                       ((1.0 * $self->{"reference blocks written"})
                        / $self->{"bios in write"})));
    $log->info(sprintf("$prefix page cache writes per data write      => \%.4f",
                       ((1.0 * $self->{"bios page cache write"})
                        / $self->{"bios in write"})));
    $log->info(sprintf("$prefix page cache reads per data write       => \%.4f",
                       ((1.0 * $self->{"bios page cache read"})
                        / $self->{"bios in write"})));
  }
}

1;
