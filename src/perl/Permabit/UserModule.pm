##
# Perl object that represents a user tools module.
#
# $Id$
##
package Permabit::UserModule;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertMinMaxArgs assertNumArgs);
use Permabit::Utils qw(makeFullPath);

use base qw(Permabit::ModuleBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

###############################################################################
# @inherit
##
sub load {
  my ($self) = assertNumArgs(1, @_);
  my $loaded = $self->SUPER::load();

  # Set the python path
  if ($loaded) {
    my $machine = $self->{machine};
    my $topdir = makeFullPath($machine->{workDir}, $self->{modVersion});
    my $link = makeFullPath($topdir, "pythonlibs");
    if ($machine->sendCommand("test -h $link")) {
      my $pythonPath = $machine->getPythonLibraryPath();
      $machine->runSystemCmd("ln -s $topdir/$pythonPath $link");
    }
  }

  return $loaded;
}

###############################################################################
# @inherit
##
sub loadFromFiles {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->{machine};
  my $modFileName = $self->{modFileName};

  if ($self->{useUpstream}) {
    $log->debug("Using upstream version VDO: $self->{modVersion}");
    my $topdir = makeFullPath($machine->{workDir}, $self->{modVersion});
    $self->_step(command => "mkdir -p $topdir");
    my $getFromDnf = join(' ',
			  "dnf", "download", "--destdir",
			  "$self->{modDir}", "$modFileName");
    $self->_step(command => $getFromDnf);
    $getFromDnf = join(' ',
                       "dnf", "download", "--destdir", "$topdir",
                       "$modFileName-support");
    $self->_step(command => $getFromDnf);
  }
  $self->SUPER::loadFromFiles();
}

###############################################################################
# @inherit
##
sub loadFromBinaryRPM {
  my ($self, $filename, $modFileName) = assertMinMaxArgs([undef], 2, 3, @_);
  $modFileName //= $self->{modFileName};

  $self->SUPER::loadFromBinaryRPM($filename);

  # Extra steps for loading the vdo-support module
  my $topdir = makeFullPath($self->{machine}->{workDir}, $self->{modVersion});
  my $supportName = "$modFileName-support";
  my $supportFile = ($self->{useDistribution}) ? makeFullPath($self->{modDir}, "$supportName-*.rpm")
                                               : makeFullPath($topdir, "$supportName-*.rpm");
  $self->SUPER::loadFromBinaryRPM($supportFile, $supportName);
}

###############################################################################
# @inherit
##
sub buildModule {
  my ($self, $modulePath, $topdir, $arch) = assertNumArgs(4, @_);

  $self->SUPER::buildModule($modulePath, $topdir, $arch);

  # Extra steps to also move the vdo-support module to the top directory
  my $modFileName = $self->{modFileName};
  my $version = $self->{modVersion};
  $self->_step(command => "mv -f $topdir/RPMS/$arch/$modFileName-support-$version*.rpm $topdir");
}

1;
