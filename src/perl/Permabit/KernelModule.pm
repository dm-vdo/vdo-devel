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
use Permabit::Assertions qw(
  assertDefined
  assertMinMaxArgs
  assertNumArgs
  assertOptionalArgs
  assertType
);
use Permabit::Constants;
use Permabit::SystemUtils qw(assertSystem);
use Permabit::Utils qw(makeFullPath);
use Permabit::VersionNumber;
use Storable qw(dclone);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %useCounts;

###############################################################################
# @paramList{new}
my %PROPERTIES
  = (
     # @ple host that needs the kernel module (a Permabit::RemoteMachine)
     machine        => undef,
     # @ple directory where the DKMS module is found
     modDir         => undef,
     # @ple kernel module name
     modName        => undef,
     # @ple kernel module version
     modVersion     => undef,
     # @ple options to pass to modprobe
     modprobeOption => "",
    );
##

###############################################################################
# Creates a C<Permabit::KernelModule>.
#
# @param params  Module properties
#
# @return a new C<Permabit::KernelModule>
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self = bless({%PROPERTIES, @_}, $invocant);
  assertDefined($self->{machine});
  assertDefined($self->{modDir});
  assertDefined($self->{modName});
  assertDefined($self->{modVersion});
  assertType("Permabit::RemoteMachine", $self->{machine});
  $self->{_cleanCommands} = [];
  return $self;
}

###############################################################################
# Load the module into the kernel for the first time
#
# @return a flag indicating if the module was actually loaded
##
sub load {
  my ($self) = assertNumArgs(1, @_);
  my $loaded = 0;
  my $machine = $self->{machine};
  my $modName = $self->{modName};
  if (!$useCounts{$machine->getName()}{$modName}) {
    $self->loadFromFiles();

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
    $loaded = 1;
  }
  ++$useCounts{$machine->getName()}{$modName};

  return $loaded;
}

###############################################################################
# Determine what type of module files are available, load them in the
# appropriate way.
##
sub loadFromFiles {
  my ($self) = assertNumArgs(1, @_);
  my $modName = $self->{modName};
  my $modVer  = $self->{modVersion};
  my $machine = $self->{machine};

  $machine->sendCommand("uname -m");
  my $arch = $machine->getStdout();
  chomp($arch);

  # Look for a binary RPM on the remote machine.
  my $binaryRPMFilename = makeFullPath($self->{modDir},
                                       "kmod-$modName-$modVer*.$arch.rpm");
  my $errno = $machine->sendCommand("test -f $binaryRPMFilename");
  if ($errno == 0) {
    $log->debug("Detected for binary RPM: $binaryRPMFilename");
    $self->loadFromBinaryRPM($binaryRPMFilename);
    return;
  }

  if (0) {
    # Look for a local SRPM to use.
    # XXX We don't support this case yet, but this is how it would work.
    my $sourceRPMFilename = makeFullPath($self->{modDir},
                                         "kmod-$modName-$modVer*.src.rpm");
    if (-f $sourceRPMFilename) {
      $log->debug("Detected source RPM: $sourceRPMFilename");
      $self->loadFromSourceRPM($sourceRPMFilename);
      return;
    }
  }

  # Look for a local tarball to use.
  my $tarFilename = makeFullPath($self->{modDir}, "$modName-$modVer.tgz");
  if (-f $tarFilename) {
    $log->debug("Detected tarball for dkms: $tarFilename");
    $self->loadFromTar($tarFilename);
    return;
  }

  # We could not file any recognizable module to load.
  die("Couldn't find any module files to install");
}

###############################################################################
# Load the module from a binary RPM on the remote host.
#
# @param filename  The name of the binary RPM to load
##
sub loadFromBinaryRPM {
  my ($self, $filename) = assertNumArgs(2, @_);
  $self->_step(command => "sudo rpm -iv $filename",
               cleaner => "sudo rpm -e kmod-kvdo");
}

###############################################################################
# Load the module from a local source RPM by building a binary and loading that.
#
# @param filename  The name of the local source RPM to load
##
sub loadFromSourceRPM {
  my ($self, $filename) = assertNumArgs(2, @_);
  # XXX This option is not implemented because we don't need it yet.
  die("Loading from source RPMs is unimplemented");

  # Generally:
  # - make sure the remote machine can build RPMS:
  # - $ yum install rpm-build redhat-rpm-config
  # - copy local source RPM to remote host (using tar, etc.)
  # -- SRPMs are usually stored in /usr/src/redhat/SRPMS
  # - Generate binary RPM with rpbbuild --rebuild: (no sudo)
  # - $ rpmbuild --rebuild <source RPM file>
  # - look in /usr/src/redhat/RPMS/<arch>/<name>.<arch>.rpm for the new module
  # - Install the new module:
  # - $ sudo rpm -iv <binary RPM file>
  # - To uninstall or clean up:
  # - $sudo rpm -e kmod-kvdo
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

###############################################################################
# Unload the module from the kernel
#
# @return a flag indicating if the module was actually unloaded
##
sub unload {
  my ($self) = assertNumArgs(1, @_);
  my $unloaded = 0;
  if (--$useCounts{$self->{machine}->getName()}{$self->{modName}} == 0) {
    $self->_cleanup();
    $unloaded = 1;
  }

  return $unloaded;
}

###############################################################################
# Run the cleanup commands
##
sub _cleanup {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->{machine};
  while (my $command = pop(@{$self->{_cleanCommands}})) {
    $machine->runSystemCmd($command);
    if ($machine->getStdout()) {
      $log->debug($machine->getStdout());
    }
  }
};

###############################################################################
# Run a step of module setup, handle problems appropriately, and schedule
# cleanup of the module.  Arguments are passed using name-value pairs.
#
# @oparam command     Setup code or command to run as part of startup.
# @oparam cleaner     Cleanup command to register upon success.
# @oparam diagnostic  Diagnostic code or command to run upon error.
#
# @croaks if the command fails
##
sub _step {
  my ($self, $args) = assertOptionalArgs(1,
                                         {
                                          command    => undef,
                                          cleaner    => undef,
                                          diagnostic => undef,
                                         },
                                         @_);
  my $machine = $self->{machine};
  my $error = undef;

  assertDefined($args->{command});
  if (ref($args->{command}) eq "CODE") {
    eval { $args->{command}->(); };
    $error = $EVAL_ERROR;
  } else {
    $log->info($machine->getName() . ": $args->{command}");
    if ($machine->sendCommand($args->{command}) != 0) {
      $log->error("Failure during $args->{command}");
      $log->error("stdout:\n" . $machine->getStdout());
      $log->error("stderr:\n" . $machine->getStderr());
      $error = "Cannot load module $self->{modName}";
    }
  }
  if ($error) {
    if (defined($args->{diagnostic})) {
      if (ref($args->{diagnostic}) eq "CODE") {
        $args->{diagnostic}->();
      } else {
        $machine->assertExecuteCommand($args->{diagnostic});
      }
    }
    $self->_cleanup();
    die($error);
  }
  if (defined($args->{cleaner})) {
    push(@{$self->{_cleanCommands}}, $args->{cleaner});
  }
}

1;
