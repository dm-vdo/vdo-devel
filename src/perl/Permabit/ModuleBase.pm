##
# Perl object that represents base functionality used by kernel and user modules.
#
# $Id$
##
package Permabit::ModuleBase;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertMinMaxArgs
  assertNe
  assertNumArgs
  assertOptionalArgs
  assertRegexpDoesNotMatch
  assertType
);
use Permabit::Utils qw(addToHash makeFullPath);

use base qw(Permabit::Propertied);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %useCounts;

###############################################################################
# @paramList{new}
my %PROPERTIES
  = (
     # @ple host that needs the module (a Permabit::RemoteMachine)
     machine         => undef,
     # @ple directory where the module is found
     modDir          => undef,
     # @ple module filename
     modFileName     => undef,
     # @ple module name
     modName         => undef,
     # @ple module version
     modVersion      => undef,
     # @ple whether the test is loading the module from a released RPM
     useDistribution => undef,
     # @ple whether the test is loading the dm-vdo module from kernel
     useUpstream     => 0,
    );
##

###############################################################################
# Create a new module object
#
# @return the module object
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self = bless({}, $class);
  addToHash($self, %{$self->cloneClassHash("PROPERTIES")}, @_);

  # We are only used as a base class.
  assertNe(__PACKAGE__, $class);

  assertDefined($self->{machine});
  assertDefined($self->{modDir});
  assertDefined($self->{modName});
  assertDefined($self->{modVersion});
  assertType("Permabit::RemoteMachine", $self->{machine});
  $self->{modFileName} //= $self->{modName};
  $self->{_cleanCommands} = [];

  return $self;
}

###############################################################################
# Load the module for the first time
#
# @return a flag indicating if the module was actually loaded
##
sub load {
  my ($self) = assertNumArgs(1, @_);
  my $loaded = 0;
  my $machine = $self->{machine};
  my $modName = $self->{modName};

  if (!$useCounts{$machine->getName()}{$modName}) {
    if (!$self->loadFromFiles()) {
      # We could not file any recognizable module to load.
      die("Couldn't find any module files to install");
    }
    $loaded = 1;
  }
  ++$useCounts{$machine->getName()}{$modName};

  return $loaded;
}

###############################################################################
# Determine what type of module files are available, load them in the
# appropriate way.
#
# @return a flag indicating success or failure
##
sub loadFromFiles {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->{machine};
  my $modFileName = $self->{modFileName};
  my $modVer = $self->{modVersion};

  $machine->sendCommand("uname -m");
  my $arch = $machine->getStdout();
  chomp($arch);

  # Look for a binary RPM on the remote machine.
  my $binaryRPMFilename = makeFullPath($self->{modDir},
                                       "$modFileName-$modVer*.$arch.rpm");
  my $errno = $machine->sendCommand("test -f $binaryRPMFilename");
  if ($errno == 0) {
    $log->debug("Detected for binary RPM: $binaryRPMFilename");
    $self->loadFromBinaryRPM($binaryRPMFilename);
    return 1;
  }

  # Look for a SRPM on the remote machine.
  my $moduleSRPMPath = makeFullPath($self->{modDir},
                                    "$modFileName-$modVer*.src.rpm");
  $errno = $machine->sendCommand("test -f $moduleSRPMPath");
  if ($errno == 0) {
    $log->debug("Detected for module source RPM: $moduleSRPMPath");
    $self->loadFromSourceRPM($moduleSRPMPath);
    return 1;
  }

  return 0;
}

###############################################################################
# Check for DKMS build failure during an installation step.
#
# @param output   The command output to examine
##
sub _checkDKMSBuildFailure {
  my ($self, $output)= assertNumArgs(2, @_);
  my $dkms_build_err = qr/Consult (\/var\/lib\/dkms\/.*\/build\/make.log) for more information/;

  if ($output =~ $dkms_build_err) {
    $log->error($output);
    my $makeLog;
    eval {
      $makeLog = $self->{machine}->cat($1);
    };
    if ($makeLog) {
      croak("rpm install logged a dkms build error: $makeLog");
    }
  }
}

###############################################################################
# Load the module from a binary RPM on the remote host.
#
# @param filename      The name of the binary RPM to load
# @oparam modFileName  The name of the module file to use in cleanup commands
##
sub loadFromBinaryRPM {
  my ($self, $filename, $modFileName) = assertMinMaxArgs([undef], 2, 3, @_);
  $modFileName //= $self->{modFileName};
  my $machine = $self->{machine};
  my $topdir = makeFullPath($machine->{workDir}, $self->{modVersion});

  if ($self->{useDistribution}) {
    $self->_step(command => "mkdir -p $topdir");
    $self->_step(command => "cp -up $filename $topdir");
    $filename = makeFullPath($topdir, "$modFileName*.rpm");
  }

  $self->_step(command => "cd $topdir && sudo rpm -iv $filename",
               cleaner => "cd $topdir && sudo rpm -e $modFileName");
  # RPM masks the DKMS exit status, but we still see the messages.
  $self->_step(command => sub {
                 my $output = $machine->getStdout() . $machine->getStderr();
                 $self->_checkDKMSBuildFailure($output);
                 assertRegexpDoesNotMatch(qr/failed/i, $output,
                                          "rpm install logged a failure");
               });
}

###############################################################################
# Build the module from source and move the result to the top level.
#
# @param modulePath  The full path of the module source RPM to load
# @param topdir      The top directory of the build tree
# @param arch        The host architecture
##
sub buildModule {
  my ($self, $modulePath, $topdir, $arch) = assertNumArgs(4, @_);
  my $machine = $self->{machine};
  my $modFileName = $self->{modFileName};
  my $version = $self->{modVersion};

  $log->debug("Building $modFileName module SRPM");
  $self->_step(command => "mkdir -p $topdir");
  $self->_step(command => "cp -up $modulePath $topdir");

  my $bindir = $self->{machine}->{userBinaryDir};
  my $buildCmd = join(' ',
                      "rpmbuild -rb --clean",
                      "--define='_topdir $topdir'",
                      "--define='_bindir $bindir'",
                      "$modulePath"
                     );
  $self->_step(command => $buildCmd);

  $self->_step(command => "mv -f $topdir/RPMS/$arch/$modFileName-$version*.rpm $topdir");
}

###############################################################################
# Load the module from a source RPM by building a binary and loading that.
#
# @param modulePath  The full path of the module source RPM to load
##
sub loadFromSourceRPM {
  my ($self, $modulePath) = assertNumArgs(2, @_);
  my $machine = $self->{machine};
  my $modFileName = $self->{modFileName};
  my $version = $self->{modVersion};
  my $topdir = makeFullPath($machine->{workDir}, $self->{modVersion});

  $machine->sendCommand("uname -m");
  my $arch = $machine->getStdout();
  chomp($arch);

  $self->buildModule($modulePath, $topdir, $arch);

  my $filename = makeFullPath($topdir, "$modFileName-$version*$arch.rpm");
  $self->loadFromBinaryRPM($filename);
}

###############################################################################
# Unload the module
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
