##
# Parser for .stats files.
#
# @synopsis
#
#   use StatsParser qw(parseStatsFile);
#
#   my $parsed = parseStatsFile('stdin', STDIN);
#
# @description
#
#  This module implements a recursive descent parser which will parse
#  .stats statistics description files into a tree of C<Statistic>
#  objects. The tree can then be processed by a set of C<Generator>
#  objects to generate statistics header definitions and parsing code.
#
# $Id$
##
package StatsParser;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Parse::RecDescent;

use Permabit::Assertions qw(
  assertMinArgs
  assertMinMaxArgs
  assertNumArgs
);

use Statistic;
use Statistic::Enum;
use Statistic::Field;
use Statistic::Struct;
use Statistic::Type;
use Statistic::Version;

use base qw(Exporter);

our @EXPORT_OK = qw(parseStatsFile);

# Sadly, because of the implementation of Parse::RecDescent, it is simplest
# to implement this single state variable as a package global.
my $TYPES;
my $PARSER;

# The grammar of .stats files.
my $GRAMMAR = q {
  Spec: 'version' Version Type(s) Statistic(s) eof
        {
          $return = StatsParser::specAction($item[4]) + $item[2];
        }
      | <error>

  Version: value end
           {
             $return = StatsParser::versionAction($item[1]);
           }
         | <error>

  Type: 'type' <commit> identifier Block
        {
          $return = StatsParser::typeAction(@item[3..4]);
        }
      | <error>

  Statistic: 'struct' <commit> Struct
           | 'versionedStruct' <commit> VersionedStruct
           | 'enum' <commit> Enum
           | identifier identifier <commit> Field
             {
               my $result = StatsParser::fieldAction(@item[1..2], $item[4]);
               if (ref($result) =~ /^Statistic/) {
                 $return = $result;
               } else {
                 _error($result, $thisline);
                 $return = undef;
               }
             }
           | identifier Block
             {
               $return = StatsParser::statisticAction(@item[1..2]);
             }
           | <error>

  Struct: identifier Block
          {
            $return = StatsParser::structAction(@item);
          }
        | <error>

  VersionedStruct: identifier Block
                   {
                     $return = StatsParser::structAction(@item);
                   }
                 | <error>

  Enum: identifier Block
        {
          $return = StatsParser::enumAction(@item[1..2]);
        }
      | <error>

  Field: Block | EmptyBlock | <error>

  Block: open AttributeList close
         {
           $return = $item[2];
         }
       | <error>

  EmptyBlock: open close
              {
                $return = [];
              }

  AttributeList: (Attribute | Statistic)(s)
                 {
                   $return = StatsParser::attributeListAction(\@item);
                 }

  Attribute: identifier value end
             {
               $return = StatsParser::attributeAction(\%item);
             }

  value:      /[^\s;\{\}][^;\{\}]*/s
  identifier: /[a-zA-Z0-9_][^;\{\}\s]*/

  eof:        /^\Z/

  open:       '{'
  close:      '}'
  end:        ';'
};

######################################################################
# Action called when parsing a statistics spec (top-level block)
#
# @param statistics  An array ref of statistics
#
# @return The spec as a Statistic
##
sub specAction {
  my ($statistics) = assertNumArgs(1, @_);
  return Statistic->new(name => 'root') + $statistics;
}

######################################################################
# Action called when parsing a type block.
#
# @param name   The name of the type
# @param block  The type definition block
#
# @return The type as a Statistic
##
sub typeAction {
  my ($name, $block) = assertNumArgs(2, @_);
  my $type = Statistic::Type->new(name => $name) + $block;
  $TYPES += $type;
  return $type;
}

######################################################################
# Action called when parsing a struct block.
#
# @param type   The type of structure (versioned or not)
# @param name   The name of the struct
# @param block  The struct definition block
#
# @return The struct as a Statistic
##
sub structAction {
  my ($structType, $name, $block) = assertNumArgs(3, @_);
  my $struct = Statistic::Struct->new(name => $name);

  # Structs are also types
  $TYPES += $struct;

  if ($structType eq 'VersionedStruct') {
    # Versioned structs have version number
    $struct += fieldAction('versionNumber', 'version');
    $struct->addAttribute('versioned', 1);
  }

  return $struct += $block;
}

######################################################################
# Action called when parsing a field block.
#
# @param type   The type of field (versioned or not)
# @param name   The name of the field
# @param block  The field definition block
#
# @return The field as a Statistic
##
sub fieldAction {
  my ($type, $name, $block) = assertMinMaxArgs([[]], 2, 3, @_);
  my $typeObject = $TYPES->getAttribute($type);
  if (!defined($typeObject)) {
    return "Unknown type: $type"
  }
  my %attributes = (type => $typeObject);

  if ($name =~ /^(.*)\[(.*)\]$/) {
    $name = $1;
    $attributes{arraySize} = $2;
  }

  $attributes{name} = $name;
  return Statistic::Field->new(%attributes) + $block;
}

######################################################################
# Action called when parsing a version declaration.
#
# @param number  The version number
#
# @return The version as a Statistic
##
sub versionAction {
  my ($number) = assertNumArgs(1, @_);
  return Statistic::Version->new($number);
}

######################################################################
# Action called when parsing an enum declaration.
#
# @param name   The name of the enum
# @param block  The enum block
#
# @return The enum as a Statistic
##
sub enumAction {
  my ($name, $block) = assertNumArgs(2, @_);
  return Statistic::Enum->new(name => $name) + $block;
}

######################################################################
# Action called when parsing a statistics group block.
#
# @param name   The name of the statistics group
# @param block  The statistics group definition block
#
# @return The statistic groups as a Statistic
##
sub statisticAction {
  my ($name, $block) = assertNumArgs(2, @_);
  return Statistic->new(name => $name) + $block;
}

######################################################################
# Action called when parsing an attribute list.
#
# @param items  The parsed array
#
# @return An array ref of attributes
##
sub attributeListAction {
  my ($items) = assertNumArgs(1, @_);
  return [@{$items->[1]}];
}

######################################################################
# Action called when parsing an attribute
#
# @param items  The attribute
#
# @return The attribute as an Attribute
##
sub attributeAction {
  my ($items) = @_;
  return bless [$items->{identifier}, $items->{value}], 'Attribute';
}

######################################################################
# Make the parser.
##
sub makeParser {
  if (defined($PARSER)) {
    return;
  }

  $PARSER = new Parse::RecDescent($GRAMMAR);
  if (!defined($PARSER)) {
    die("Bad grammar!");
  }
}

######################################################################
# Parse a .stats file.
#
# @param sourceFile  The name of the .stats file
# @param fh          The file handle from which to read
# @oparam parameters Optional parameters for controlling the diagonstics from
#                    Parse::RecDescent
#
# @return The parse tree as a tree of C<Statistic> objects.
##
sub parseStatsFile {
  my ($sourceFile, $fh, %parameters) = assertMinArgs(2, @_);
  makeParser();

  # pre-process the input
  my $input = '';
  while (my $line = $fh->getline()) {
    # strip comments
    $line =~ s/#.*$//;
    $input .= $line;
  }

  $::RD_HINT  = $parameters{hint};
  $::RD_TRACE = $parameters{trace};
  $::RD_WARN  = $parameters{warn};
  $TYPES      = Statistic::Type->new(name => 'types');

  my $parsed = $PARSER->Spec($input);
  if (!defined($parsed)) {
    die("\n");
  }

  $parsed->addAttribute('sourceFile', $sourceFile);
  return $parsed;
}

1;
