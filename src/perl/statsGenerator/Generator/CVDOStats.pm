##
# C file generator for vdoStats output
#
# @synopsis
#
#     use Generator::CVDOStats
#
#
# @description
#
# C<Generator::CVDOStats> generates C files from a parsed .stats file.
#
# $Id$
##
package Generator::CVDOStats;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);
use List::Util qw(max);
use Text::Diff;

use Permabit::Assertions qw(
  assertMinArgs
  assertNumArgs
);

use base qw(Generator);

##
# @paramList{new}
our %PROPERTIES
  = (
     # @ple The total count of lines to output
     fieldCount          => 0,
     # @ple The language this generator generates
     language            => 'CVDOStats',
     # @ple The attribute which is the output file name for this generator
     outputFileAttribute => 'cvdostatsOutput',
     # @ple The set of unique structs
     structs             => {},
     # @ple The unique types
     types               => {},
    );

######################################################################
# This function figures out what label to use for this field,
#
# @param field The field to get the label for.
#
# @return The label to use
##
sub calculateLabel {
  my ($self, $field) = assertNumArgs(2, @_);
  my $type = $field->getType("C");
  my $subStruct = $field->getType("user") !~ qr/%/;

  my $label = undef;
  if ($subStruct) {
    my $struct = $self->{structs}{$type};
    my $labelPrefix = $struct->getAttribute('labelPrefix');
    if (defined($labelPrefix)) {
      $label
        = join(' ', grep { defined($_) } ($self->{labelPrefix}, $labelPrefix));
    }
    $labelPrefix = $field->getAttribute('labelPrefix');
    if (defined($labelPrefix)) {
      $label
        = join(' ', grep { defined($_) } ($self->{labelPrefix}, $labelPrefix));
    }
    if (!defined($label)) {
      return undef;
    }
  } else {
    $label = $field->getAttribute('label');
    if (!defined($label)) {
      $label = $field->{name};
      # Convert "launchVDOErrorCount" to "launch VDO error count"
      $label =~ s/([A-Z][a-z])/ \l$1/g;
      $label =~ s/([a-z])([A-Z])/$1 $2/g;
    }
  }
  $label =~ s/(\(%\))/(%%)/g;

  return $label;
}

######################################################################
# Do some initial calculation of unique types and field count.
#
# @param statistic  The statistic to process
##
sub calculateStats {
  my ($self, $statistic) =  assertNumArgs(2, @_);

  if ($self->shouldSkip($statistic)) {
    return;
  }

  if ($statistic->isa("Statistic::Field")) {
    my $type = $statistic->getType("C");
    my $pct = $statistic->getType("user");
    if ($pct =~ qr/%/) {
      $self->{fieldCount}++;
      if (!defined($self->{types}{$type})) {
        $self->{types}{$type} = $statistic;
        return;
      }
    } else {
      my $struct = $self->{structs}{$type};
      $self->calculateStats($struct);
    }
  }

  if ($statistic->isa("Statistic::Struct")) {
    $self->{structs}{$statistic->{name}} = $statistic;
  }

  foreach my $child ($statistic->getChildren()) {
    $self->calculateStats($child);
  }

}

######################################################################
# This function takes some c-like pseudo code from VDOStatistics.stats
# and converts it to be used by the actual C code, by replacing field
# names with the actual C field names in statistics.h
#
# @param struct The struct the fields are in
# @param pseudo The pseudo code to convert
#
# @return The converted code.
##
sub convertToCode {
  my ($self, $struct, $pseudo) = assertNumArgs(3, @_);
  $pseudo //= "";

  foreach my $name (keys(%{$struct->{derived}})) {
    $pseudo =~ s/\$$name/$name/g;
  }
  $pseudo =~ s/(\$)/stats->/g;
  $pseudo =~ s/(.)([A-Z][a-z]+)/$1_$2/g;
  $pseudo =~ s/([a-z0-9])([A-Z])/$1_$2/g;
  $pseudo = lc($pseudo);
  return $pseudo;
}

######################################################################
# Emit a C comment.
#
# @param statistic  The statistic whose comment is to be printed
##
sub emitComment {
  my ($self, $statistic) = assertNumArgs(2, @_);
  my $comment            = $statistic->getAttribute('comment');
  if (!defined($comment)) {
    return;
  }

  my @comment = split("\n", $comment);
  if (@comment == 1) {
    $self->emit("/** $comment */");
    return;
  }

  $self->emit('/**');
  $self->indent(' * ');
  map { $self->emit($_) } @comment;
  $self->undent();
  $self->emit(' **/');
}

######################################################################
# Emit a derived field at the top of the struct function so that
# the field emiiter code can use that field directly.
#
# @param struct The structure the derived field is part of.
# @param field  The derived field to emit.
##
sub emitDerived {
  my ($self, $struct, $field) = assertNumArgs(3, @_);
  my $derived = $field->getAttribute("cderived");
  if (!defined($derived)) {
    return;
  }

  # If its already been emitted, ignore
  if (defined($struct->{derived}{$field->{name}})) {
    return;
  }

  # Handle derived fields inside derived fields so they're done first.
  my $derivedFrom = $self->getDerivedFromFields($struct, $derived);
  foreach my $fromField (@{$derivedFrom}) {
    $self->emitDerived($struct, $fromField);
  }

  $struct->{derived}{$field->{name}} = 1;

  my $type = $field->getType("C");
  my $name = $self->camelcaseToKernelStyle($field);
  my $code = $self->convertToCode($struct, $derived);

  my $arraySize = $field->getArraySize();
  if (defined($arraySize)) {
    $self->replaceAndEmit("TYPE NAMEARRAY = \"\";",
                          "TYPE", $type,
                          "NAME", $name,
                          "ARRAY", "[" . $arraySize . "]");
    $self->replaceAndEmit("sprintf(NAME, \"%s\", CODE);",
                          "NAME", $name,
                          "CODE", $code);
  } else {
    $self->replaceAndEmit("TYPE NAME = CODE;",
                          "TYPE", $type,
                          "NAME", $name,
                          "CODE", $code);
  }
}

######################################################################
# Emit derived fields in a struct
#
# @param struct  The struct to output derived fields for.
##
sub emitDerives {
  my ($self, $struct) = assertNumArgs(2, @_);
  foreach my $field ($struct->getChildren()) {
    $self->emitDerived($struct, $field);
  }
}

######################################################################
# Emits a series of lines that will write a given field.
#
# @param field  the field to write out
##
sub emitField {
  my ($self, $field) = assertNumArgs(2, @_);

  my $type = $field->getType("C");
  my $realType = $type =~ "char" ? "string" : $type;
  my $name = $self->camelcaseToKernelStyle($field);
  my $subStruct = $field->getType("user") !~ qr/%/;
  my $derived = $field->getAttribute("cderived");

  $self->blankLine();
  $self->emitComment($field);

  my $label = $self->calculateLabel($field);
  if (!defined($label)) {
    # Use camelcase name for label since perl and python look for it
    $self->emit("if (asprintf(&joined, \"%s\", prefix) == -1) {");
  } else {
    # Use camelcase name for label since perl and python look for it
    $self->replaceAndEmit("if (asprintf(&joined, \"%s LABEL\", "
                          . "prefix) == -1) {",
                          "LABEL", $label);
  }
  $self->indent();
  $self->emit("return VDO_UNEXPECTED_EOF;");
  $self->undent();
  $self->emit("}");

  my $avail = $field->getAttribute('cavailable');
  if (defined($avail)) {
    my $code = $self->convertToCode($field->{parent}, $avail);
    $self->replaceAndEmit("if (CODE) {", "CODE", $code);
    $self->indent();
  }

  if ($subStruct) {
    my $struct = $self->{structs}{$type};
    $realType = $self->camelcaseToKernelStyle($struct);
    $self->replaceAndEmit("result = write_TYPE(joined, &stats->STATNAME);",
                          "TYPE", $realType,
                          "STATNAME", $name);
  } else {
    # Use camelcase name for label since perl and python look for it
    $self->replaceAndEmit("result = write_TYPE(joined, PREFIXSTATNAME);",
                          "TYPE", $realType,
                          "PREFIX", defined($derived) ? "" : "stats->",
                          "STATNAME", $name);
  }

  if (defined($avail)) {
    $self->undent();
    $self->emit("} else {");
    $self->indent();
    # Use camelcase name for label since perl and python look for it
    $self->emit("result = write_string(joined, \"N/A\");");
    $self->undent();
    $self->emit("}");
  }

  $self->emit("free(joined);");

  $self->emit("if (result != VDO_SUCCESS) {");
  $self->indent();
  $self->emit("return result;");
  $self->undent();
  $self->emit("}");

}

######################################################################
# Emit a C separator.
##
sub emitSeparator {
  my ($self) = assertNumArgs(1, @_);
  $self->emit("/*******************************************************"
              . "***************/");
}

######################################################################
# Emits a function that will generate all the output for a given struct
#
# @param struct the structure to write out
##
sub emitStruct {
  my ($self, $struct) = assertNumArgs(2, @_);

  my $name = $self->camelcaseToKernelStyle($struct);

  $self->blankLine();
  $self->emitSeparator();
  $self->replaceAndEmit("static int write_STRUCT(char *prefix,",
                        "STRUCT", $name);
  $self->indentTo("static int write_${name}(");
  $self->replaceAndEmit("struct STRUCT *stats)", "STRUCT", $name);
  $self->undent();
  $self->emit("{");
  $self->indent();
  $self->emit("int result = 0;");
  $self->emit("char *joined = NULL;");
  $self->blankLine();

  $self->emitDerives($struct);

  foreach my $field ($struct->getChildren()) {
    $self->generate($field);
  }

  $self->emit("return VDO_SUCCESS;");
  $self->undent();
  $self->emit("}");
}

######################################################################
# Emits a function that will create the output for a unique field type.
# This doesn't actually print anything. It merely stores the label in
# one array and the value in the other. This way we can build up the
# max label length so we can properly align all the fields afterwards.
#
# @param field the field to create a type function for
##
sub emitType {
  my ($self, $field) = assertNumArgs(2, @_);

  my $type = $field->getType("C");
  my $funcType = $type =~ "char" ? "string" : $type;
  my $cType = $type =~ "char" ? "char *" : $type . " ";

  $self->blankLine();
  $self->emitSeparator();
  $self->replaceAndEmit("static int write_TYPE(char *label, CVALvalue)",
                        "TYPE", $funcType,
                        "CVAL", $cType);
  $self->emit("{");
  $self->indent();
  $self->emit("int count = sprintf(labels[fieldCount], \"%s\", label);");
  $self->emit("if (count < 0) {");
  $self->indent();
  $self->emit("return VDO_UNEXPECTED_EOF;");
  $self->undent();
  $self->emit("}");

  $self->blankLine();
  $self->emit("maxLabelLength = max(maxLabelLength, (int) strlen(label));");
  $self->blankLine();

  $self->replaceAndEmit("count = sprintf(values[fieldCount++], "
                        . "\"PCT\", value);",
                        "PCT", $field->getType("user"));
  $self->emit("if (count < 0) {");
  $self->indent();
  $self->emit("return VDO_UNEXPECTED_EOF;");
  $self->undent();
  $self->emit("}");
  $self->emit("return VDO_SUCCESS;");
  $self->undent();
  $self->emit("}");
}

######################################################################
# Emits all unique types
##
sub emitTypes {
  my ($self) = assertNumArgs(1, @_);
  foreach my $type (keys %{$self->{types}}) {
    $self->emitType($self->{types}{$type});
  }
}

######################################################################
# Given a field name, find the actaul field statistic.
#
# @param struct The structure the field is in
# @param fieldName The name of the field to find
##
sub findField {
  my ($self, $struct, $fieldName) = assertNumArgs(2, @_);
  foreach my $field ($struct->getChildren()) {
    if ($fieldName eq $field->{name}) {
      return $field;
    }
  }
  return undef;
}

######################################################################
# @inherit
##
sub generateHeader {
  my ($self, $group) = assertNumArgs(2, @_);

  $self->calculateStats($group);

  $self->generateSPDX($group);
  $self->emit('/*');
  $self->indent(' * ');
  $self->SUPER::generateHeader($group);
  $self->undent();
  $self->emit(' */');
  $self->blankLine();

  my $headerText = <<"EOH";
#include <stdlib.h>

#include "math.h"
#include "statistics.h"
#include "status-codes.h"
#include "vdoStats.h"

EOH
  $self->blankLine();
  $self->emit($headerText);
  $self->blankLine();
  $self->replaceAndEmit("#define MAX_STATS COUNT",
			"COUNT", $self->{fieldCount});
  $self->emit("#define MAX_STAT_LENGTH 80");
  $self->blankLine();
  $self->emit("int fieldCount = 0;");
  $self->emit("int maxLabelLength = 0;");
  $self->blankLine();
  $self->emit("char labels[MAX_STATS][MAX_STAT_LENGTH];");
  $self->emit("char values[MAX_STATS][MAX_STAT_LENGTH];");

  $self->blankLine();
  $self->emitTypes();
  $self->blankLine();
}

######################################################################
# @inherit
##
sub generateEnum {
  my ($self, $enum) = assertNumArgs(2, @_);
}

######################################################################
# @inherit
##
sub generateField {
  my ($self, $field) = assertNumArgs(2, @_);

  if ($self->shouldSkip($field)) {
    return;
  }

  $self->emitField($field);
}

######################################################################
# @inherit
##
sub generateStruct {
  my ($self, $struct) = assertNumArgs(2, @_);
  $self->emitStruct($struct);
}

######################################################################
# @inherit
##
sub generateTrailer {
  my ($self, $statistic) = assertNumArgs(2, @_);
  $self->blankLine();
  $self->emitSeparator();
  $self->emit("int vdo_write_stats(struct vdo_statistics *stats)");
  $self->emit("{");
  $self->indent();
  $self->emit("fieldCount = 0;");
  $self->emit("maxLabelLength = 0;");
  $self->blankLine();
  $self->emit("memset(labels, '\\0', MAX_STATS * MAX_STAT_LENGTH);");
  $self->emit("memset(values, '\\0', MAX_STATS * MAX_STAT_LENGTH);");
  $self->blankLine();
  $self->emit("int result = write_vdo_statistics(\" \", stats);");
  $self->emit("if (result != VDO_SUCCESS) {");
  $self->indent();
  $self->emit("return result;");
  $self->undent();
  $self->emit("}");

  # This loop is where we actually print out the stats.
  $self->emit("for (int i = 0; i < fieldCount; i++) {");
  $self->indent();
  $self->emit("printf(\"%s%*s : %s\\n\",");
  $self->indentTo("printf(");
  $self->emit("labels[i],");
  $self->emit("maxLabelLength - (int) strlen(labels[i]), \"\",");
  $self->emit("values[i]);");
  $self->undent();
  $self->undent();
  $self->emit("}");

  $self->emit("return VDO_SUCCESS;");
  $self->undent();
  $self->emit("}");
}

######################################################################
# Get the list of fields from the cavailable attribute.
#
# @param struct The structure the derived fields are in.
# @param pseudo The c pseudo code for how the field is derived.
##
sub getDerivedFromFields {
  my ($self, $struct, $pseudo) = assertNumArgs(3, @_);
  my @fromFields = ( $pseudo =~ /.*$(.*+) .*/g );

  @fromFields = map { $self->findField($struct, $_) } @fromFields;
  return \@fromFields;
}

######################################################################
# @inherit
##
sub shouldSkip {
  my ($self, $statistic) = assertNumArgs(2, @_);
  # determine whether to skip stats
  if ((($statistic->{name} eq 'version')
       || ($statistic->{name} eq 'releaseVersion'))
      && ($statistic->checkAttribute('displayVersion', 'False', 1))) {
    return 1;
  }

  my $display = $statistic->getAttribute('display');
  if (defined($display) && ($display eq 'False')) {
    # We don't ever want to output this field.
    return 1;
  }

  return ($self->SUPER::shouldSkip($statistic));
}

1;
