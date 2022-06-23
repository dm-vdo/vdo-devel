######################################################################
# A package that exports the values defined in the CURRENT_VERSION
# file for perl scripts to use. This module allow the values defined
# in CURRENT_VERSION to be overridden. This is useful for our release
# builds where the project version number gets set buy the build software.
#
# @synopsis
#
#   use Permabit::Version qw($VDO_VERSION);
#   print "Albireo's version is $VDO_VERSION\n";
#
# @description
#
# C<Permabit::Version> provides programatic way of accessing the values
# inside of the CURRENT_VERSION file.
#
# For accessing the CURRENT_VERSION values though this module in perl,
# it's as simple as importing scalar constants of the same name. The
# values in the CURRENT_VERSION file can be overridden by setting
# environment variables of the same name to the desired value.
#
# $Id$
##
package Permabit::Version;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp qw(confess);
use File::Spec;
use FindBin;
use IO::File;

use feature qw(state);

use base qw(Exporter);

our @EXPORT_OK = qw(
  getDefaultTopdir
  getDefaultCurrentVersionFile
);

# TODO: * Add a way to have this module ignore the values of the
#         environment variables.
#       * Have a way to save out the current values of the variables
#         so tagAndArchive.pl doesn't have to duplicate this code

our $CUR_VER_FILE = File::Spec->catfile(getDefaultTopdir(),
                                        'src/tools/installers',
                                        'CURRENT_VERSION');
_parseFile($CUR_VER_FILE);

#############################################################################
# Take the values from the CURRENT_VERSION file add its vars to our export
# list.
##
sub _parseFile {
  my $fileName = shift;
  my $fh = IO::File->new($fileName, 'r')
    or confess("can't open $fileName: $OS_ERROR");

  my @variables = map { _tokenizeLine($_) } <$fh>;
  foreach my $ref (@variables) {
    my ($varName, $op, $value) = @$ref;
    no strict 'refs';
    no warnings 'once';
    $$varName = ($op eq '?=' && exists($ENV{$varName})) ? $ENV{$varName}
                                                        : $value;
    push(@EXPORT_OK, "\$$varName");
  }
}

######################################################################
# Split up a Makefile variable assignment into three tokens:
#   * the variable name
#   * the assignment operator (=, :=, etc)
#   * the value it is being set to
#
# @param  line           The line to tokenize
#
# @return arrayref of tokens or nothing if the line doesn't look
#         like a Makefile variable assignment.
##
sub _tokenizeLine {
  my $line = shift;
  if ($line =~ /^\s*(\S+)        # The variable name
                \s*([:+?]?=)\s*  # =, :=, +=, or ?=
                (.*)$/x) {       # Everything else
    return [$1, $2, $3];
  }
  return;
}

#############################################################################
# Get the default current version file.
#
# @return the default current version file
##
sub getDefaultCurrentVersionFile {
  return $CUR_VER_FILE;
}

#############################################################################
# Get the default topdir.
#
# @return the default topdir
##
sub getDefaultTopdir {
  state $bin;
  if (!$bin) {
    $bin = $FindBin::Bin;
    $bin =~ s{^(.*)(src|main)/(tools|perl)/.*?$}{$1};
  }
  return $bin;
}

1;
