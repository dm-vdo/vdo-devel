#!/usr/bin/perl

##
# Transform upstream patches for vdo-devel.
#
# @synopsis
#
# transformUpstreamPatch.pl <patch-file>
#
# @level{+}
#
# @item patch-file
#
# The name of the patch file to transform.
#
# @level{-}
#
# @description
#
# Transform upstream patches from the directory structure in the DM tree to
# match the one in our vdo-devel tree.
##

use strict;
use warnings FATAL => qw(all);
use English;

if (!$ARGV[0]) {
  print "At least one file argument is required.\n";
  exit();
}

my @udsFiles = qw(
  /chapter-index\.[hc]
  /config\.[hc]
  /cpu\.h
  /delta-index\.[hc]
  /errors\.[hc]
  /funnel-queue\.[hc]
  /funnel-requestqueue\.h
  /geometry\.[hc]
  /hash-utils\.h
  /index\.[hc]
  /indexer\.h
  /index-layout\.[hc]
  /index-page-map\.[hc]
  /index-session\.[hc]
  /io-factory\.[hc]
  /logger\.h
  /memory-alloc.h
  /murmurhash3\.[hc]
  /numeric\.h
  /open-chapter\.[hc]
  /permassert\.[hc]
  /radix-sort\.[hc]
  /sparse-cache\.[hc]
  /string-utils\.[hc]
  /thread-utils\.h
  /time-utils\.h
  /volume\.[hc]
  /volume-index\.[hc]
);

my @udsKernelFiles = qw(
  /funnel-requestqueue\.c
  /logger\.c
  /memory-alloc\.c
  /thread-device\.[hc]
  /thread-registry\.[hc]
  /thread-utils\.c
);

my @upstreamFiles = qw(
  Makefile
  Kconfig
);

my $udsFilesPattern = join('|', @udsFiles);
my $udsFilesRE = qr/$udsFilesPattern/;
my $udsKernelFilesPattern = join('|', @udsKernelFiles);
my $udsKernelFilesRE = qr/$udsKernelFilesPattern/;
my $upstreamFilesPattern = join('|', @upstreamFiles);
my $upstreamFilesRE = qr/$upstreamFilesPattern/;

my $args = join(' ', @ARGV);
my @files = glob($args);

foreach my $file (@files) {
  print "Transforming $file\n";
  rename($file, "$file.orig");
  open(IN, "$file.orig") || die("Couldn't open $file\n");
  open(OUT, ">$file") || die();

  while (my $line = <IN>) {
    if ($line =~ m|dm-vdo/$upstreamFilesRE|) {
      $line =~ s|drivers/md/dm-vdo/($upstreamFilesRE)|src/packaging/kpatch/$1.upstream|g;
    } elsif ($line =~ /$udsFilesRE/) {
      $line =~ s|drivers/md/dm-vdo/indexer|src/c++/uds/src/uds|g;
      $line =~ s|drivers/md/dm-vdo|src/c++/uds/src/uds|g;
    } elsif ($line =~ /$udsKernelFilesRE/) {
      $line =~ s|drivers/md/dm-vdo/indexer|src/c++/uds/kernelLinux/uds|g;
      $line =~ s|drivers/md/dm-vdo|src/c++/uds/kernelLinux/uds|g;
    } elsif ($line =~ m|drivers/md/dm-vdo|) {
      # If we have a new file, let's guess it should go with VDO, unless it's
      # in the indexer subdirectory in which case it's probably UDS.
      $line =~ s|drivers/md/dm-vdo/indexer|src/c++/uds/src/uds|g;
      $line =~ s|drivers/md/dm-vdo|src/c++/vdo/base|g;
    }

    print OUT $line;
  }

  close(IN);
  close(OUT);
  unlink("$file.orig");
}
