##
# C header file generator for statistics.
#
# @synopsis
#
#     use Generator::C
#
#
# @description
#
# C<Generator::C> generates C header files from a parsed .stats file.
#
# $Id$
##
package Generator::C;

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
     language => 'C',
    );

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
  $define            =~ tr/.-/__/;
  return uc($define);
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

  my $define = $self->getDefineVariable($group);
  $self->emit("#ifndef $define");
  $self->emit("#define $define");
  $self->blankLine();

  my $includes = $group->getAttribute('cIncludes');
  if ($includes) {
    map { $self->emit("#include $_") }
      map { $self->quoteString($_) }
        split(/\s*,\s*/, $includes);
    $self->blankLine();
  }

  $self->emitVersion($group);
}

######################################################################
# @inherit
##
sub emitVersion {
  my ($self, $group) = assertNumArgs(2, @_);
  my $versionEnum = $group->getAttribute('cVersionEnum');
  if (!defined($versionEnum)) {
    return;
  }
  $self->SUPER::emitVersion($group);
}

######################################################################
# @inherit
##
sub generateEnum {
  my ($self, $enum) = assertNumArgs(2, @_);
  $self->printComment($enum);
  $self->emit("enum {");
  $self->indent();
  foreach my $enumerant ($enum->getEnumerants()) {
    my $value = $enum->getAttribute($enumerant);
    $self->emit("$enumerant = $value,");
  }
  $self->undent();
  $self->emit('};');
  $self->blankLine();
}

######################################################################
# @inherit
##
sub generateStruct {
  my ($self, $struct) = assertNumArgs(2, @_);
  $self->printComment($struct);

  my $name = $self->camelcaseToKernelStyle($struct);
  $self->emit("struct $name {");
  $self->indent();
  foreach my $field ($struct->getChildren()) {
    $self->generate($field);
  }
  $self->undent();
  $self->emit("};");
  $self->blankLine();
}

######################################################################
# @inherit
##
sub generateField {
  my ($self, $field) = assertNumArgs(2, @_);
  $self->printComment($field);

  my $type      = $self->camelcaseToKernelStyle($field->{type});
  my $subStruct = ($field->getType("kernel") !~ qr/%/);
  my $arraySize = $field->getArraySize();
  my $name      = $self->camelcaseToKernelStyle($field);
  if (defined($arraySize)) {
    $name .= "[$arraySize]";
  }
  if ($subStruct) {
    $self->emit("struct $type $name;");
  } else {
    $self->emit("$type $name;");
  }
}

######################################################################
# Print a C comment.
#
# @param statistic  The statistic whose comment is to be printed
##
sub printComment {
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
# @inherit
##
sub generateTrailer {
  my ($self, $statistic) = assertNumArgs(2, @_);
  my $define = $self->getDefineVariable($statistic);
  $self->emit("#endif /* not $define */");
}

1;
