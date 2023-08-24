##
# C header file generator for statistics.
#
# @synopsis
#
#     use Generator::CMessage
#
#
# @description
#
# C<Generator::CMessage> generates C header files from a parsed .stats file.
#
# $Id$
##
package Generator::CMessage;

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
     language            => 'CMessage',
     # @ple The attribute which is the output file name for this generator
     outputFileAttribute => 'cmessageOutput',
    );

######################################################################
# Emit type handlers for all the types used by a statistic.
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
    my $pct = $statistic->getType("kernel");
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
    $self->emit("/* $comment */");
    return;
  }

  $self->emit('/*');
  $self->indent(' * ');
  map { $self->emit($_) } @comment;
  $self->undent();
  $self->emit(' */');
}

######################################################################
# Emits a series of lines that will write a given field to an output
# buffer and do some checkin of the result.
#
# @ple field the field to write out
##
sub emitField {
  my ($self, $field) = assertNumArgs(2, @_);

  $self->emitComment($field);

  my $type = $self->camelcaseToKernelStyle($field->{type});
  my $realType = $type =~ "char" ? "string" : $type;
  my $name = $self->camelcaseToKernelStyle($field);
  my $subStruct = $field->getType("kernel") !~ qr/%/;
  # Use camelcase name for label since perl and python look for it
  $self->replaceAndEmit("result = write_TYPE(\"PREFIX\",",
                        "TYPE", $realType,
                        "PREFIX", $field->{name} . " : ");
  $self->indentTo("result = write_${realType}(");
  $self->replaceAndEmit("SUBstats->STATNAME,",
                        "SUB", $subStruct ? "&" : "",
                        "STATNAME", $name);
  $self->replaceAndEmit("\"SUFFIX\",", "SUFFIX", ", ");
  $self->emit("buf,");
  $self->emit("maxlen);");
  $self->undent();
  $self->emit("if (result != VDO_SUCCESS)");
  $self->indent();
  $self->emit("return result;");
  $self->undent();
}

######################################################################
# Emits a function that will write a given structure to the output
# buffer.
#
# @ple struct the structure to write out
##
sub emitStruct {
  my ($self, $struct) = assertNumArgs(2, @_);

  $self->blankLine();
  my $name = $self->camelcaseToKernelStyle($struct);
  $self->replaceAndEmit("static int write_STRUCT(char *prefix,",
                        "STRUCT", $name);
  $self->indentTo("static int write_${name}(");
  $self->replaceAndEmit("struct STRUCT *stats,", "STRUCT", $name);
  $self->emit("char *suffix,");
  $self->emit("char **buf,");
  $self->emit("unsigned int *maxlen)");
  $self->undent();
  $self->emit("{");
  $self->indent();
  $self->emit("int result;");
  $self->blankLine();
  $self->emit("result = write_string(prefix, \"{ \", NULL, buf, maxlen);");
  $self->emit("if (result != VDO_SUCCESS)");
  $self->indent();
  $self->emit("return result;");
  $self->undent();

  foreach my $field ($struct->getChildren()) {
    $self->generate($field);
  }

  $self->emit("result = write_string(NULL, \"}\", suffix, buf, maxlen);");
  $self->emit("if (result != VDO_SUCCESS)");
  $self->indent();
  $self->emit("return result;");
  $self->undent();
  $self->emit("return VDO_SUCCESS;");
  $self->undent();
  $self->emit("}");
}

######################################################################
# Emits a function that will write a specific type to the output
# buffer.
#
# @ple field the field to create a type function for
##
sub emitType {
  my ($self, $field) = assertNumArgs(2, @_);

  $self->blankLine();

  my $type = $field->getType("C");
  my $funcType = $type =~ "char" ? "string" : $type;
  my $cType = $type =~ "char" ? "char *" : $type . " ";
  $self->replaceAndEmit("static int write_TYPE(char *prefix,",
                        "TYPE", $funcType);
  $self->indentTo("static int write_${funcType}(");
  $self->replaceAndEmit("TYPEvalue,", "TYPE", $cType);
  $self->emit("char *suffix,");
  $self->emit("char **buf,");
  $self->emit("unsigned int *maxlen)");
  $self->undent();
  $self->emit("{");
  $self->indent();

  $self->replaceAndEmit("int count = scnprintf(*buf, *maxlen, \"%sPCT%s\",",
                        "PCT", $field->getType("kernel"));
  $self->indentTo("int count = scnprintf(");
  $self->emit("prefix == NULL ? \"\" : prefix,");
  $self->emit("value,");
  $self->emit("suffix == NULL ? \"\" : suffix);");
  $self->undent();
  $self->emit("*buf += count;");
  $self->emit("*maxlen -= count;");
  $self->emit("if (count >= *maxlen)");
  $self->indent();
  $self->emit("return VDO_UNEXPECTED_EOF;");
  $self->undent();
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
#include "dedupe.h"
#include "logger.h"
#include "memory-alloc.h"
#include "message-stats.h"
#include "statistics.h"
#include "thread-device.h"
#include "vdo.h"

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
    $self->replaceAndEmit("int STAT_write_stats(struct vdo *vdo,", "STAT",
                          $type);
    $self->indentTo("int write_${type}_stats(");
    $self->emit("char *buf,");
    $self->emit("unsigned int maxlen)");
    $self->undent();
    $self->emit("{");
    $self->indent();
    my $name = $self->camelcaseToKernelStyle($struct);
    $self->replaceAndEmit("struct STRUCT *stats;", "STRUCT", $name);
    $self->emit("int result;");
    $self->blankLine();
    $self->replaceAndEmit("result = UDS_ALLOCATE(1, struct STRUCT,"
                          . " __func__, &stats);", "STRUCT", $name);
    $self->emit("if (result != VDO_SUCCESS)");
    $self->indent();
    $self->emit("return result;");
    $self->undent();
    $self->blankLine();
    $self->emit("vdo_fetch_statistics(vdo, stats);");
    $self->replaceAndEmit("result = write_STRUCT(NULL, stats, NULL, &buf,"
                          . " &maxlen);",
                          "STRUCT", $name);
    $self->emit("uds_free(stats);");
    $self->emit("return result;");
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
