#!/usr/bin/perl

##
# Create a source distribution.
#
# @synopsis
#
# prepareSourceDist.pl [--dest=<destination directory>]
#                      [--manifest=<manifest file>]
#                      [--changelog=<changelog file>]
#                      [--author=<changelog author>]
#                      [--debug]
#                      <source-tree>
#
# @level{+}
#
# @item dest B<destination directory>
#
# The directory in which the tarballs will be created. Defaults to the current
# directory.
#
# @item manifest B<manifest file>
#
# The path to the manifest file. If relative, it will be taken relative to the
# top of the source tree. Defaults to src/packaging/src-dist/MANIFEST.yaml.
#
# @item changelog B<changelog file>
#
# The path to a file containing changelog entries to be added to the spec
# files. Each entry in the file consists of the name of a spec file on a line
# by itself, followed by the changelog entry for that file, followed by a blank
# line or EOF. Any occurance of the string '$VERSION' will be replaced by
# the release version.
#
# @item author B<changelog author>
#
# The author of this distribution for inclusion in spec file change log
# entries. Defaults to $ENV{user}@`hostname`.
#
# @item debug
#
# If present, more verbose logging will occur, and cleanups will not be done on
# failure.
#
# @item source-tree
#
# The top of the source tree or tarball from which to create the distribution.
#
# @level{-}
#
# @description
#
# prepareSourceDist.pl prepares the tarballs which are the inputs to building
# source distribution builds. A manifest file describes which files are to be
# included and also defines some transformations of those files. The syntax of
# the manifest file is documented at the end of C<Permabit::DistFramework>
#
# $Id$
##

use strict;
use warnings FATAL => qw(all);
use English;

use FindBin;
# Use the current real value of lastrun so that things don't change if the
# symlink gets re-pointed.
use lib (defined($ENV{PERMABIT_PERL_PATH}))
         ? split('\s*,\s*', $ENV{PERMABIT_PERL_PATH})
         : "${FindBin::RealBin}/../commonlib";
use lib "${FindBin::RealBin}/../lib";

use Permabit::DistFramework::SRPM;

our $log;

Permabit::DistFramework::SRPM->run();
