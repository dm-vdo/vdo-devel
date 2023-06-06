##
# Perl object that represents a kernel module in a DKMS package.
#
# $Id$
##
package Permabit::KernelModule;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::SystemUtils qw(assertSystem);
use Permabit::Utils qw(makeFullPath);
use Permabit::VersionNumber;

use base qw(Permabit::ModuleBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

###############################################################################
# @paramList{new}
our %PROPERTIES
  = (
     # @ple options to pass to modprobe
     modprobeOption => "",
    );
##

###############################################################################
# @inherit
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self = $class->SUPER::new(@_);

  if ($self->{modFileName} =~ /^kvdo$/) {
    $self->{modFileName} = 'kmod-' . $self->{modName};
  }

  return $self;
}

###############################################################################
# @inherit
##
sub load {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->{machine};
  my $modName = $self->{modName};

  if ($self->SUPER::load()) {
    # modprobe will tell us whether we managed to load successfully.
    # modprobe failures leave the useful logging in the log, so
    # capture the kernel log upon any failure.
    my $cursor = $machine->getKernelJournalCursor();
    my $uponModprobeError = sub {
      $machine->syncKernLog();
      my $additions = $machine->getKernelJournalSince($cursor);
      if ($additions) {
        $log->debug("kernel log additions:\n$additions");
      }
    };
    $self->_step(command => "sudo modprobe $modName $self->{modprobeOption}",
                 cleaner => "sudo modprobe -r $modName",
                 diagnostic => $uponModprobeError);
  }

  return (!$machine->runSystemCmd("sudo modinfo $modName"));
}

###############################################################################
# @inherit
##
sub loadFromFiles {
  my ($self) = assertNumArgs(1, @_);
  my $modName = $self->{modName};
  my $modVer = $self->{modVersion};

  my $loaded = $self->SUPER::loadFromFiles();
  if ($loaded == 0) {
    # Look for a local tarball to use.
    my $tarFilename = makeFullPath($self->{modDir}, "$modName-$modVer.tgz");
    if (-f $tarFilename) {
      $log->debug("Detected tarball for dkms: $tarFilename");
      $self->loadFromTar($tarFilename);
      $loaded = 1;
    }
  }

  return $loaded;
}

###############################################################################
# Load the module from a local tarball using dkms.
#
# @param filename  The name of the local tarball to load from
##
sub loadFromTar {
  my ($self, $filename) = assertNumArgs(2, @_);
  my $modName = $self->{modName};
  my $modVer  = $self->{modVersion};
  my $machine = $self->{machine};

  my $unpack = sub {
    # assertSystem lets us redirect standard input from a local file
    assertSystem("ssh $SSH_OPTIONS "
                 . $machine->getName()
                 . " sudo tar xvfz - -C /usr/src < $filename");
  };
  $self->_step(command => $unpack,
               cleaner => "sudo rm -fr /usr/src/$modName-$modVer");

  $self->_step(command => "sudo dkms add $modName/$modVer",
               cleaner => ("sudo dkms remove $modName/$modVer --all"
                           . " && sync"));

  my $makeLog = "/var/lib/dkms/$modName/$modVer/build/make.log";
  $self->_step(command    => "sudo dkms install $modName/$modVer",
               diagnostic => "cat $makeLog");

  # Try to flush the contents of the modules files out to disk in case of a
  # crash.
  #
  # If the modules files are updated but not flushed to disk and then we crash,
  # we can be left with empty files, which causes the machine to not boot
  # properly (failing any NFS mounts because the kernel module isn't known)
  # until depmod can be manually re-run.
  $machine->assertExecuteCommand("sync");
}

###############################################################################
# Load the module into the kernel after a reboot
##
sub reload {
  my ($self) = assertNumArgs(1, @_);
  my $modName = $self->{modName};
  $self->_step(command => "sudo modprobe $modName $self->{modprobeOption}");
}

1;
