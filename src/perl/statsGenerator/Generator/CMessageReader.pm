##
# C file generator for reading dmsetup message stats output.
#
# @synopsis
#
#     use Generator::CMessageReader
#
#
# @description
#
# C<Generator::CMessageReader> generates C files from a parsed .stats file
#
# $Id$
##
package Generator::CMessageReader;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);

use Permabit::Assertions qw(
  assertMinArgs
  assertNumArgs
);

use base qw(Generator);

##
# @paramList{new}
our %PROPERTIES
  = (
     # @ple The language this generator generates
     language            => 'CMessageReader',
     # @ple The attribute is the output file name for this generator
     outputFileAttribute => 'cmessagereaderOutput',
    );

######################################################################
# Emit type functions for all the unique types used by the statistics.
#
# @param statistic  The statistic to process
# @param typesSeen  A hashref containing all types already processed
##
sub emitTypes {
  my ($self, $statistic, $typesSeen) =  assertNumArgs(3, @_);

  if ($self->shouldSkip($statistic)) {
    return;
  }

  if ($statistic->isa("Statistic::Field")) {
    my $pct = $statistic->getType("user");
    if ($pct =~ qr/%/) {
      my $type = $statistic->getType("C");
      if (!defined($typesSeen->{$type})) {
        $typesSeen->{$type} = $statistic;
        $self->emitType($statistic);
        return;
      }
    }
  }

  foreach my $child ($statistic->getChildren()) {
    $self->emitTypes($child, $typesSeen);
  }
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
  $self->emit(' */');
}

######################################################################
# Emits a series of lines that will read a given field from an input
# buffer and do some checking of the result.
#
# @ple field the field to read
##
sub emitField {
  my ($self, $field) = assertNumArgs(2, @_);

  $self->emitComment($field);

  my $type = $self->camelcaseToKernelStyle($field->{type});
  my $realType = $type =~ "char" ? "string" : $type;
  my $address = $type =~ "char" ? "" : "&";
  my $name = $self->camelcaseToKernelStyle($field);
  my $subStruct = $field->getType("user") !~ qr/%/;

  $self->replaceAndEmit("result = skip_string(buf, \"NAME : \");",
                        "NAME", $field->{name});
  $self->emit("if (result != VDO_SUCCESS) {");
  $self->indent();
  $self->emit("return result;");
  $self->undent();
  $self->emit("}");

  # Use camelcase name for label since perl and python look for it
  $self->replaceAndEmit("result = read_TYPE(buf,", "TYPE", $realType);
  $self->indentTo("result = read_${realType}(");
  $self->replaceAndEmit("ADDRESSstats->STATNAME);",
                        "ADDRESS", $address,
                        "STATNAME", $name);
  $self->undent();
  $self->emit("if (result != VDO_SUCCESS) {");
  $self->indent();
  $self->emit("return result;");
  $self->undent();
  $self->emit("}");
}

######################################################################
# Emits a function that will read a given structure from the input
# buffer.
#
# @ple struct the structure to read
##
sub emitStruct {
  my ($self, $struct) = assertNumArgs(2, @_);

  $self->blankLine();
  my $name = $self->camelcaseToKernelStyle($struct);
  $self->replaceAndEmit("static int read_STRUCT(char **buf,",
                        "STRUCT", $name);
  $self->indentTo("static int read_${name}(");
  $self->replaceAndEmit("struct STRUCT *stats)", "STRUCT", $name);
  $self->undent();
  $self->emit("{");
  $self->indent();

  $self->emit("int result = 0;");
  $self->blankLine();
  foreach my $field ($struct->getChildren()) {
    $self->generate($field);
  }

  $self->emit("return VDO_SUCCESS;");
  $self->undent();
  $self->emit("}");
}

######################################################################
# Emits a function that will read a specific type from an input
# buffer.
#
# @ple field the field to create a type function for
##
sub emitType {
  my ($self, $field) = assertNumArgs(2, @_);

  $self->blankLine();

  my $type = $field->getType("C");
  my $funcType = $type =~ "char" ? "string" : $type;
  my $cType = "$type *";

  $self->replaceAndEmit("static int read_TYPE(char **buf,",
                        "TYPE", $funcType);
  $self->indentTo("static int read_${funcType}(");
  $self->replaceAndEmit("CTYPEvalue)", "CTYPE", $cType);
  $self->undent();
  $self->emit("{");
  $self->indent();

  if ($type =~ "bool") {
    $self->emit("int temp;");
    $self->emit("int count = sscanf(*buf, \"%d, \", &temp);");
    $self->emit("*value = (bool)temp;");
  } elsif ($type =~ "char") {
    $self->emit("int count = sscanf(*buf, \"%[^,], \", value);");
  } else {
    $self->replaceAndEmit("int count = sscanf(*buf, \"PCT, \", value);",
                          "PCT", $field->getType("user"));
  }
  $self->emit("if (count != 1) {");
  $self->indent();
  $self->emit("return VDO_UNEXPECTED_EOF;");
  $self->undent();
  $self->emit("}");

  $self->emit("return VDO_SUCCESS;");
  $self->undent();
  $self->emit("}");
}

######################################################################
# Derive the symbol to use for the #ifndef wrapper of a c header file.
#
# @param group  The group being generated
##
sub getDefineVariable {
  my ($self, $group) =  assertNumArgs(2, @_);
  my $define         =  $group->getAttribute('cHeader', 1);
  $define            =~ s/[A-Z][a-z]/_$&/g;
  $define            =~ s/([a-z])([A-Z])/$1_$2/g;
  $define            =~ tr/./_/;
  return uc($define);
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
sub generateHeader {
  my ($self, $group) = assertNumArgs(2, @_);
  $self->generateSPDX($group);
  $self->emit('/*');
  $self->indent(' * ');
  $self->SUPER::generateHeader($group);
  $self->undent();
  $self->emit(' */');
  $self->blankLine();

  my $headerText = <<"EOH";
#include "statistics.h"
#include "status-codes.h"
#include "vdoStats.h"

static int skip_string(char **buf, char *skip)
{
        char *tmp = NULL;
        tmp = strstr(*buf, skip);
        if (tmp == NULL) {
                return VDO_UNEXPECTED_EOF;
        }
        *buf = tmp + strlen(skip);
        return VDO_SUCCESS;
}

EOH
  $self->emit($headerText);

  my $typesSeen = {};
  $self->emitTypes($group, $typesSeen);
}

######################################################################
# @inherit
##
sub generateField {
  my ($self, $field) = assertNumArgs(2, @_);
  $self->emitField($field);
}

######################################################################
# @inherit
##
sub generateStruct {
  my ($self, $struct) = assertNumArgs(2, @_);
  $self->emitStruct($struct);

  if ($struct->{name} eq "VDOStatistics") {
    $self->blankLine();
    my $type = $struct->{parent}->{name};
    my $name = $self->camelcaseToKernelStyle($struct);
    $self->replaceAndEmit("int read_STAT_stats(char *buf,", "STAT",
                          $type);
    $self->indentTo("int read_${type}_stats(");
    $self->replaceAndEmit("struct STRUCT *stats)", "STRUCT", $name);
    $self->undent();
    $self->emit("{");
    $self->indent();
    $self->replaceAndEmit("return(read_STRUCT(&buf, stats));",
                          "STRUCT", $name);
    $self->undent();
    $self->emit("}");
  }
}

######################################################################
# @inherit
##
sub generateTrailer {
  my ($self, $statistic) = assertNumArgs(2, @_);
}

1;
