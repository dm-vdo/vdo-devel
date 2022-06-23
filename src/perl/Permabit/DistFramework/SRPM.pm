##
# Prepare a SRPM distribution.
#
# @synopsis
#
# use Permabit::Dist::SRPM
#
# Permabit::Dist::SRPM->run();
#
# @description
#
# C<Permabit::Dist::SRPM> prepares sources to be built into a set of SRPMs.
#
# $Id$
##
package Permabit::DistFramework::SRPM;

use strict;
use warnings FATAL => qw(all);
use English;

use File::Basename;

use Permabit::Assertions qw(assertNumArgs);

use base qw(Permabit::DistFramework);

my $MANIFEST_FILE = 'src/packaging/src-dist/MANIFEST.yaml';

######################################################################
# @inherit
##
sub run {
  my ($package) = assertNumArgs(1, @_);
  $package->SUPER::run(config => { manifest => $MANIFEST_FILE });
}

######################################################################
# @inherit
##
sub processChangeLog {
  my ($self, $handle) = assertNumArgs(2, @_);
  my $specFile;
  my $entry = '';
  while (my $line = $handle->getline()) {
    if ($specFile) {
      if ($line !~ /^\s*$/) {
        $entry .= $line;
        next;
      }

      if ($entry) {
        $self->set("${specFile}ChangeLog", $entry);
        $entry = '';
      }
      $specFile = undef;
      next;
    }

    if ($line =~ /^\s*$/) {
      next;
    }

    if ($line =~ /^(\S+\.spec)/) {
      $specFile = $1;
      next;
    }

    die("Invalid changelog file, expected a spec file but got $line\n");
  }
}

######################################################################
# @inherit
##
sub updateSpecFileChangeLog {
  my ($self, $specFileName, $spec, $header) = assertNumArgs(4, @_);
  # If a change log entry was given, add it.
  my $change = $self->get(basename($specFileName) . 'ChangeLog');
  if (defined($change)) {
    $spec =~ s/(%changelog)/$1\n$header\n$change/s;
  }
  return $spec;
}

######################################################################
# @inherit
##
sub makeTarball {
  my ($self, $tarballType) = assertNumArgs(2, @_);
  my $tarballDir = basename($self->get("${tarballType}Dir"));
  my $tarball    = basename($tarballDir) . '.tgz';
  my $command    = "tar czf $tarball $tarballDir";
  $self->debug("$command");
  if (system($command) != 0) {
    die("Tar of $tarballDir failed\n");
  }
  $self->addCleanup(sub { unlink($tarball) }, onError => 1);
}

1;
