#!/usr/bin/perl

##
# Compile .stats files into C and Perl code.
#
# @synopsis
#
# generateStatsCode.pl --languages=I[C,Perl]
#                      [--ioctls=I<ioctls file>]
#                      [--help]
#                      I<stats file>...
#
# @level{+}
#
# @item B<--ioctls> I<ioctls file>
#
# A file which maps ioctl names to the ioctl value. This file is only necessary
# if the stats file specifies ioctls for non-C code.
#
# @item B<--languages>
#
# The languages for which to generate code. Currently supported options are
# C and Perl.
#
# @item B<--help>
#
# Print help text and exit.
#
# @item I<stats file...>
#
# One or more .stats files which specify the code to be generated.
#
# @level{-}
#
# @description
#
# generateStatsCode.pl compiles .stats files into C and Perl code for
# describing, parsing, and displaying statistics.
#
# $Id$
##
use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Cwd qw(cwd);
use FindBin;

# Use the current real value of lastrun so that things don't change if the
# symlink gets re-pointed.
use lib (
  defined($ENV{PERMABIT_PERL_PATH})
          ? split('\s*,\s*', $ENV{PERMABIT_PERL_PATH})
          : "${FindBin::RealBin}/../commonlib",
  cwd(),
  $FindBin::RealBin,
  "${FindBin::RealBin}/../lib",
);

use Getopt::Long;
use IO::File;

use Pdoc::Generator qw(pdoc2help pdoc2usage);
use Permabit::Assertions qw(
  assertNumArgs
);

# Uncomment this to get a stack trace on errors.
#use Carp;
#$SIG{ __DIE__ } = sub { Carp::confess( @_ ) };

use Generator::C;
use Generator::CMessage;
use Generator::CMessageReader;
use Generator::CSysfs;
use Generator::CVDOStats;
use Generator::Perl;
use StatsParser qw(parseStatsFile);

my $help = 0;
my $ioctlsFile;
my $languages;

if (!GetOptions("help"        => \$help,
                "ioctls:s"    => \$ioctlsFile,
                "languages:s" => \$languages)) {
  pdoc2usage();
}

if ($help) {
  pdoc2help();
  exit(0);
}

my %generatorArgs;
if (defined($ioctlsFile)) {
  my $ioctlsFH = IO::File->new($ioctlsFile);
  if (!defined($ioctlsFH)) {
    die("failed to open ioctls file $ioctlsFile: $ERRNO\n");
  }
  my $ioctls = { map({ chomp($_); split(':', $_) } $ioctlsFH->getlines()) };
  $generatorArgs{ioctls} = $ioctls;
  $generatorArgs{releaseVersion} = delete $ioctls->{RELEASE_VERSION};
}

my @inputFiles
  = map { [$_, IO::File->new($_)] || die("Failed to open $_: $ERRNO") } @ARGV;
if (!@inputFiles) {
  @inputFiles = (['stdin', *STDIN]);
}

my @generators = map {
  my $class = 'Generator::' . $_;
  $class->new(%generatorArgs);
} split(',', $languages);

foreach my $stat (map { parseStatsFile(@{$_}) } @inputFiles) {
  foreach my $generator (@generators) {
    $generator->generate($stat);
  }
}

