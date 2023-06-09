##
# Generate a perl class representing a set of statistics structures.
#
# @synopsis
#
#     use Generator::Perl
#
#
# @description
#
# C<Generator::Perl> generates perl classes from Statistics objects
#
# $Id$
##
package Generator::Perl;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);

use Data::Dumper;

use Permabit::Assertions qw(
  assertMinArgs
  assertNumArgs
);

use base qw(Generator);

my $PERL_HEADER = << "EOHT";
A hash which describes the statistics from auto-generated code.

\@synopsis

    use PACKAGE
    \$DEFINITIONS->{StatisticsVersion}

\@description

This auto-generated package provides a hashref which describes all of the
fields from a set of auto-generated statistics.
EOHT

my $PERL_INCLUDES = << "EOIT";
package PACKAGE;

use strict;

use base qw(Exporter);

our \@EXPORT = qw(\$DEFINITIONS);
EOIT

##
# @paramList{new}
our %PROPERTIES
  = (
     # @ple The language this generator generates
     language    => 'Perl',
     # @ple The current label prefix
     labelPrefix => undef,
     # @ple The current stat group
     statGroup   => undef,
     # @ple The set of stat groups
     statGroups  => undef,
     # @ple The set of structures
     structures  => undef,
    );

######################################################################
# @inherit
##
sub generateHeader {
  my ($self, $statistic) =  assertNumArgs(2, @_);
  my $header             =  $PERL_HEADER;
  my $package            =  $statistic->getAttribute('perlPackage', 1);
  my $definitions        =  $statistic->getAttribute('perlDefinitions', 1);
  $header                =~ s/PACKAGE/$package/g;
  $header                =~ s/DEFINITIONS/$definitions/g;

  $self->generateSPDX($statistic);
  $self->emit('##');
  $self->indent('# ');
  map { $self->emit($_) } split("\n", $header);
  $self->blankLine(1);
  $self->generateAttention($statistic);
  $self->undent();
  $self->emit('##');

  my $includes = $PERL_INCLUDES;
  $includes =~ s/PACKAGE/$package/g;
  $includes =~ s/DEFINITIONS/$definitions/g;
  $self->emit($includes);
  $self->blankLine();

  my $version = $statistic->getAttribute('version', 1);
  foreach my $enumerant ($version->getEnumerants()) {
    if ($enumerant eq 'STATISTICS_VERSION') {
      $self->{statGroups}{StatisticsVersion}
        = $version->getAttribute($enumerant);
      return;
    }
  }
}

######################################################################
# @inherit
##
sub generate {
  my ($self, $statistic) = assertNumArgs(2, @_);
  my $display = $statistic->getAttribute('display');
  if (defined($display) && ($display eq 'False')) {
    # Display of False implies that perl code will never see this stat
    return;
  }

  $self->SUPER::generate($statistic);
}

######################################################################
# @inherit
##
sub generateEnum {
  my ($self, $enum) = assertNumArgs(2, @_);
  if ($enum->{name} ne 'ReleaseVersions') {
    # No enums in perl other than release versions which are special
    return;
  }

  my $current = 'VDO_CURRENT_RELEASE_VERSION_NUMBER';
  while (defined($current) && ($current !~ /^\d+$/)) {
    $current = $enum->getAttribute($current);
  }

  if (defined($current)) {
    $self->{statGroups}{ReleaseVersion} = $current;
  }
}

######################################################################
# Prepare to record a new struct, and, if it is designated as a top-level
# 'perlStat', note that as well.
##
sub setStatGroupAndStruct {
  my ($self, $struct) = assertNumArgs(2, @_);
  my $perlStat = $struct->getAttribute('perlStat');
  if (defined($perlStat)) {
    my $statGroup = $self->{statGroups}{$perlStat};
    if (!defined($statGroup)) {
      $statGroup = $self->{statGroups}{$perlStat} = [];
    }
    $self->{statGroup} = $statGroup;
  }

  $self->{structures}{$struct->{name}} = $struct;
}

######################################################################
# @inherit
##
sub generateStruct {
  my ($self, $struct) = assertNumArgs(2, @_);
  # This function doesn't actually generate any code. If we are inside a stat
  # group, we will record the hierarchy of structures and fields for later
  # dumping. Otherwise, we will just make a note that this struct type exists
  # and when we encounter it while recording a hierarchy, will find it and
  # expand it then.

  my $statGroupSet = defined($self->{statGroup});
  if (!$statGroupSet) {
    # If we aren't yet inside a top-level perlStat, see if we should be.
    $self->setStatGroupAndStruct($struct);
  }

  if (!defined($self->{statGroup})) {
    # If we aren't inside a top-level perlStat, there's nothing more to do.
    return;
  }

  foreach my $field ($struct->getChildren()) {
    $self->generate($field);
  }

  if (!$statGroupSet) {
    # We have now finished the top level perlStat, so clear the fact that
    # we were in it.
    $self->{statGroup} = undef;
  }
}

######################################################################
# @inherit
##
sub generateField {
  my ($self, $field) = assertNumArgs(2, @_);
  my $type           = $self->getType($field);
  my $struct         = $self->{structures}{$type};
  if (defined($struct)) {
    # This field is actually a sub-structure, so expand it now.
    my $previousLabelPrefix = $self->{labelPrefix};
    my $labelPrefix = $struct->getAttribute('labelPrefix');
    if (defined($labelPrefix)) {
      $self->{labelPrefix}
        = join(' ', grep { defined($_) } ($self->{labelPrefix}, $labelPrefix));
    }
    $labelPrefix = $field->getAttribute('labelPrefix');
    if (defined($labelPrefix)) {
      $self->{labelPrefix}
        = join(' ', grep { defined($_) } ($self->{labelPrefix}, $labelPrefix));
    }

    $self->generateStruct($struct);

    $self->{labelPrefix} = $previousLabelPrefix;
    return;
  }

  # This is an actual, bottom-level field, so record its attributes.
  if (($field->{name} eq 'version')
      && ($field->checkAttribute('displayVersion', 'False', 1))) {
    return;
  }

  my $label = $field->getAttribute('label');
  if (!defined($label)) {
    $label = $field->{name};
    # Convert "launchVDOErrorCount" to "launch VDO error count"
    $label =~ s/([A-Z][a-z])/ \l$1/g;
    $label =~ s/([a-z])([A-Z])/$1 $2/g;
  }
  if (defined($self->{labelPrefix})) {
    $label = "$self->{labelPrefix} $label";
  }
  push(@{$self->{statGroup}}, [$label, $type, $field->getAttribute('unit'),
                               $field->getAttribute('perlLabel')]);
}

######################################################################
# @inherit
##
sub generateTrailer {
  my ($self, $statistic) = assertNumArgs(2, @_);

  # Now that we've record all the attributes, dump them into the definitions
  # hash.
  my $definitions = $statistic->getAttribute('perlDefinitions', 1);
  $self->emit('our '
              . Data::Dumper->Dump([$self->{statGroups}], [$definitions]));
  $self->blankLine();
  $self->emit('1;');
}

1;
