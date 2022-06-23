#!/usr/bin/perl

##
# Create a distribution for pushing to GitHub.
#
# @synopsis
#
# prepareGitHubDist.pl [--dest=<destination directory>]
#                      [--manifest=<manifest file>]
#                      [--changelog=<changelog file>]
#                      [--author=<changelog author>]
#                      [--repositoryPrefix=<repository prefix>]
#                      [--branch=<branch>]
#                      [--kernel=<kernel version>]
#                      [--fromRPMs]
#                      [--debug]
#                      [--test]
#                      <source-tree>
#
# @level{+}
#
# @item B<dest> I<destination directory>
#
# The directory in which the local git repositories will be created.
#
# @item B<manifest> I<manifest file>
#
# The path to the manifest file. If relative, it will be taken relative to the
# top of the source tree. Defaults to src/packaging/src-dist/MANIFEST.yaml.
#
# @item B<changelog> I<changelog file>
#
# The path to a file containing the text to be used for the git commit message.
# This text will also be used as the changelog for any spec files.
#
# @item B<author> I<changelog author>
#
# The author of this distribution for inclusion in spec file change log
# entries. Defaults to $ENV{user}@`hostname`.
#
# @item B<repositoryPrefix> I<repository prefix>
#
# The repository prefix for the projects. For example, for the kvdo and vdo
# GitHub projects, the repository prefix is dm-vdo.
#
# @item B<branch> I<branch>
#
# The git branch on which to commit. If omitted, the correct branch will be
# inferred from the version.
#
# @item B<kernel> I<kernel version>
#
# The kernel version to assign to the LINUX_VERSION_CODE macro when
# preprocessing the source, in the form N.N.N.
#
# @item B<fromRPMs> I< >
#
# This flag indicates that the distribution should be created from the set
# of SRPMs in the source-tree directory. The sources are already prepared
# for distribution and need only be extracted, merged into the appropriate
# git trees, and to have the commit message generated from the changelog.
#
# @item B<debug> I< >
#
# If present, more verbose logging will occur, and cleanups will not be done on
# failure.
#
# @item B<test> I< >
#
# If present, this is a test build. Do not actually interact with GitHub, just
# prepare the sources.
#
# @item source-tree I< >
#
# The top of the source tree or tarball from which to create the distribution.
#
# @level{-}
#
# @description
#
# prepareGitHubDist.pl prepares sources to be pushed to GitHub for
# distribution.  A manifest file describes which files are to be included and
# also defines some transformations of those files. The syntax of the manifest
# file is documented at the end of C<Permabit::DistFramework>.
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

use Permabit::DistFramework::GitHub;

our $log;

Permabit::DistFramework::GitHub->run();
