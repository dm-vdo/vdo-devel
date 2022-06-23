##
# Utilities for working with VDO statistics
#
# $Id$
##
package Permabit::Statistics::Utils;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Permabit::Assertions qw(
  assertNumArgs
);

use base qw(Exporter);

our @EXPORT_OK = qw(
  getStatisticsMaps
  getStatisticsFieldNames
  prepareStatisticsProperties
);

my $NAME  = 0;
my $TYPE  = 1;
my $UNIT  = 2;
my $TITLE = 3;

######################################################################
# Get the field names for a given stats type.
#
# @param definitions  The statistics definitions
# @param name  	      The name of the stats type
#
# @return An arrayref containing the field names for the given type,
#         in the order produced by the python stats tools
##
sub getStatisticsFieldNames {
  my ($definitions, $name) = assertNumArgs(2, @_);
  return [ map { $_->[$NAME] } @{$definitions->{$name}} ];
}

######################################################################
# Prepare the class properties based on the stats definitions supplied.
#
# @param definitions  A Statistics definition hash provided statically or
#                     loaded from a generated file.
# @param mapName      The name of the statistics sub-object in the definitions
#                     hash.
#
# @return A hash usually assigned to %PROPERTIES for the class in question.
##
sub prepareStatisticsProperties {
  my ($definitions, $mapName) = assertNumArgs(2, @_);

  my ($fieldNames, $typeMap, $unitsMap, $titleMap)
    = getStatisticsMaps($definitions, $mapName);

  my @functions = qw(getWritesOnDisk
                     getReadsOnDisk
                     getPageCacheWritesOnDisk
                     getPageCacheReadsOnDisk
                     getJournalWritesOnDisk
                     getJournalReadsOnDisk
                     getIOsOnDisk);

  foreach my $function (@functions) {
    $unitsMap->{$function} = 'Count';
  }

  # Note that dclone is not used for the properties; the *Map hashes are
  # intended to be shared.

  return (definitions       => $definitions,
          typeMap           => $typeMap,
          unitsMap          => $unitsMap,
          titleMap          => $titleMap,
          fieldNames        => $fieldNames,
          fieldCount        => scalar(@{$fieldNames}),
          _assignDefaultValues($typeMap));
}

######################################################################
# Construct the hash of default values for fields.
#
# @param typeMap  The hash of types
#
# @return a list of fieldname to default value pairs
##
sub _assignDefaultValues {
  my ($typeMap) = assertNumArgs(1, @_);
  return map({ ($_, _assignDefaultValue($_, $typeMap)) }
	     keys(%{$typeMap}));
}

######################################################################
# Assign the default value of a stat.
#
# @param name     The name of the stat
# @param typeMap  The hash of field types.
#
# @return The default value of that statistic
##
sub _assignDefaultValue {
  my ($name, $typeMap) = assertNumArgs(2, @_);
  my $type = $typeMap->{$name};
  if ($type eq 'hash') {
    return {};
  }

  if ($type eq 'state') {
    return undef;
  }

  return 0;
}

######################################################################
# Make a map from a statistics definition.
#
# @param definitions  The statistics definitions
# @param name  	      The name of the stats type
# @param index 	      The index in a sta
#
# @return A hashref mapping field names to the given property for these fields
#         which define the property
##
sub makeStatisticsMap {
  my ($definitions, $name, $index) = assertNumArgs(3, @_);
  return { map { ($_->[$NAME], $_->[$index]) }
           grep { defined($_->[$index]) }
           @{$definitions->{$name}}
         };
}

######################################################################
# Get an array consisting of the field names array, each of the maps
# for a given stats type.
#
# @param definitions  The statistics definitions
# @param name         The name of the stats type
#
# @return An array containing the field names list, and the type, unit, and
#         title maps for a given stats type
##
sub getStatisticsMaps {
  my ($definitions, $name) = assertNumArgs(2, @_);
  return ( getStatisticsFieldNames($definitions, $name),
           map { makeStatisticsMap($definitions, $name, $_) }
	   ($TYPE, $UNIT, $TITLE) );
}

1;
