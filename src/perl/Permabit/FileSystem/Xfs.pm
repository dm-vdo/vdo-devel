##
# Perl object that represents an Xfs filesystem on a Block device
#
# $Id$
##
package Permabit::FileSystem::Xfs;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Basename qw(basename);
use Permabit::Assertions qw(assertFalse assertMinMaxArgs assertNumArgs);
use Permabit::PlatformUtils qw(isMaipo);
use Permabit::Utils qw(makeFullPath);
use Storable qw(dclone);

use base qw(Permabit::FileSystem);

#############################################################################
# @paramList{new}
my %PROPERTIES = (
                  # @ple we are a type xfs fileSystem
                  fsType => "xfs",
                  # @ple the mkfs.xfs options
                  mkfsOptions  => [ "-f", "-q", "-K", ],
                  # @ple mount using the discard option
                  mountOptions => [ "discard" ],
                 );
##

#############################################################################
# Creates a C<Permabit::FileSystem::Xfs>.
#
# @params{new}
#
# @return a new C<Permabit::FileSystem::Xfs>
##
sub new {
  my $invocant = shift;
  return $invocant->SUPER::new(%{ dclone(\%PROPERTIES) },
                               # Overrides previous values
                               @_,);
}

#############################################################################
# @inherit
##
sub logCopy {
  my ($self, $toDir) = assertNumArgs(2, @_);
  my $machine = $self->getMachine();
  my $path    = $self->{device}->getSymbolicPath();
  my $logPath = makeFullPath($toDir, basename($path));
  $machine->runSystemCmd("sudo xfs_logprint -C '$logPath' $path");
}

#############################################################################
# @inherit
##
sub getBlockSizeMkfsOption {
  my ($self) = assertNumArgs(1, @_);
  # Xfs has a wrong-headed block size specification.
  return "-b size=$self->{blockSize}";
}

#############################################################################
# @inherit
##
sub mount {
  my ($self, $mountDir) = assertMinMaxArgs([undef], 1, 2, @_);
  $self->SUPER::mount($mountDir);
  
  # XFS's default behavior when encountering ENOSPC/EIO is to retry
  # infinitely; this is configurable on kernels 4.7 and up.
  # On RHEL7, we configure it to retry only once.
  my $machine = $self->getMachine();
  if (isMaipo($machine->getName())) {
    my $sysfsDir = "/sys/fs/xfs/" . basename($self->{device}->getDevicePath())
                   . "/error/metadata";
    $machine->runSystemCmd("echo 1 | sudo tee $sysfsDir/EIO/max_retries");
    $machine->runSystemCmd("echo 1 | sudo tee $sysfsDir/ENOSPC/max_retries");
  }
}

#############################################################################
# @inherit
##
sub resizefs {
  my ($self) = assertNumArgs(1, @_);
  assertFalse($self->{exported});
  my $didMount = 0;

  # Unlike every other filesystem, Xfs demands to be mounted to grow.
  if (!$self->{mounted}) {
    $self->mount();
    $didMount = 1;
  }
  my $resizeCommand = "sudo xfs_growfs -d " . $self->getMountDir();
  $self->{machine}->runSystemCmd($resizeCommand);
  if ($didMount) {
    $self->unmount();
  }
}

1;
