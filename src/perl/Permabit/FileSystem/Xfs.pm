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
use Log::Log4perl;
use Permabit::Assertions qw(assertFalse assertMinMaxArgs assertNumArgs);
use Permabit::PlatformUtils qw(isMaipo);
use Permabit::Utils qw(makeFullPath);
use Storable qw(dclone);

use base qw(Permabit::FileSystem);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

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
# Per-machine setup (once per test) of XFS error_level
##
sub _ensureXfsErrorLevelConfigured {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getMachine();

  if (!$machine->{_xfs_error_level_configured}) {
    # Ensure xfs sysctl is present (ignore failures)
    $machine->sendCommand("sudo modprobe xfs");

    # Read and remember current value
    $machine->runSystemCmd("sysctl -n fs.xfs.error_level");
    my $originalErrorLevel = $machine->getStdout();
    chomp($originalErrorLevel);

    $machine->{_xfs_error_level_configured} = 1;

    # Restore on cleanup; log failures, don’t croak
    $machine->addCleanupStep(sub {
      my ($userMachine) = assertNumArgs(1, @_);
      local $EVAL_ERROR;
      eval {
        $userMachine->runSystemCmd("sudo sysctl -w fs.xfs.error_level=$originalErrorLevel");
      };
      if ($EVAL_ERROR) {
        $log->warn("Failed to restore fs.xfs.error_level to $originalErrorLevel on "
                   . $userMachine->getName() . ": $EVAL_ERROR");
      }
    });

    # Set desired runtime value
    $machine->runSystemCmd("sudo sysctl -w fs.xfs.error_level=11");
  }
}

#############################################################################
# @inherit
##
sub mount {
  my ($self, $mountDir) = assertMinMaxArgs([undef], 1, 2, @_);

  # Ensure XFS error level is configured once per machine
  $self->_ensureXfsErrorLevelConfigured();

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
