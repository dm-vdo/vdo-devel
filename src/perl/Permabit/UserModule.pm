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
sub loadFromBinaryRPM {
  my ($self, $filename, $modFileName) = assertMinMaxArgs([undef], 2, 3, @_);
  $modFileName //= $self->{modFileName};

  $self->SUPER::loadFromBinaryRPM($filename);

  # FIXME: Extra steps to load vdo-support module if it is not already installed
  # Necessary for how RegenerateGeometry operates.
  my $machine = $self->{machine};
  my $topdir = makeFullPath($self->{machine}->{workDir}, $self->{modVersion});
  my $supportName = "$modFileName-support";
  my $supportFile = makeFullPath($topdir, "$supportName-*.rpm");

  $machine->sendCommand("rpm -qa $supportName");
  if ($machine->getStdout() !~ /^$supportName/) {
    $self->SUPER::loadFromBinaryRPM($supportFile, $supportName);
  } else {
    push(@{$self->{_cleanCommands}}, "cd $topdir && sudo rpm -e $supportName");
  }
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
