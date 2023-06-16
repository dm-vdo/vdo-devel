##############################################################################
# A command for running the vdo command, the VDO Manager
#
# $Id$
##
package Permabit::CommandString::VDO;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp;
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants;

use base qw(Permabit::CommandString);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %COMMANDSTRING_PROPERTIES
  = (
     # the number of threads to use for acknowledging write requests
     ackThreads          => undef,
     # whether or not to enable the VDO volume or start it
     activate            => undef,
     # operate on all configured VDO devices
     all                 => undef,
     # the number of I/O operations to submit in a batch before moving to the
     # next bio submission thread
     bioRotationInterval => undef,
     # the number of threads to use to submit storage I/O operations
     bioThreads          => undef,
     # a string describing the size of the block map cache
     blockMapCacheSize   => undef,
     # the block map period
     blockMapPeriod      => undef,
     # the command to run, one of create, remove, start, stop, enable, disable,
     # status, list, growLogical, growPhysical, printConfigFile
     command             => undef,
     # whether to enable or disable compression
     compression         => undef,
     # the number of threads to use for cpu-intensive work
     cpuThreads          => undef,
     # whether to enable or disable deduplication
     deduplication       => undef,
     # Run with debug logging
     debug               => 0,
     # the storage device for the VDO volume
     device              => undef,
     # the list of extensions to disable
     disableExtensions   => undef,
     # forcibly unmount file systems and remove
     force               => undef,
     # forcibly rebuild
     forceRebuild        => undef,
     # the number of hash lock zones/threads for subdividing work
     hashZoneThreads     => undef,
     # print help message and exit
     help                => undef,
     # the index memory size
     indexMem            => undef,
     # if specified, the path to use for a log file
     logfile              => undef,
     # the number of logical zones/threads for subdividing work
     logicalThreads      => undef,
     # max discard size
     maxDiscardSize      => undef,
     # executable name
     name                => "vdo",
     # Print commands instead of executing them
     noRun               => undef,
     # print pending modifications to the device in square brackets
     pending             => undef,
     # the number of physical zones/threads for subdividing work
     physicalThreads     => undef,
     # whether the albireo index is sparse
     sparseIndex         => undef,
     # log level of the VDO driver
     vdoLogLevel         => undef,
     # a string describing the logical size of the VDO volume
     vdoLogicalSize      => undef,
     # The size of the VDO slabs
     vdoSlabSize         => undef,
     # print commands before executing them
     verbose             => undef,
     # version number of VDO manager
     version             => undef,
    );

our %COMMANDSTRING_INHERITED_PROPERTIES
  = (
     # The block map era length
     blockMapPeriod => undef,
     # The configuration file
     confFile       => undef,
     # Name of the VDO device to operate on
     deviceName     => undef,
     # Whether to emulate a 512 byte block device
     emulate512     => undef,
     # The path to the python libraries
     pythonLibDir   => undef,
     # Needed for create and import commands
     uuid           => undef,
     # The identifier for this VDO
     vdoIdentifier   => undef,
    );

###############################################################################
# @inherit
##
sub getEnvironment {
  my ($self) = assertNumArgs(1, @_);
  $self->{env}->{PYTHONDONTWRITEBYTECODE} = "true";
  if (defined($self->{pythonLibDir})) {
    $self->{env}->{PYTHONPATH} = "$self->{pythonLibDir}:\$PYTHONPATH";
  }

  return $self->SUPER::getEnvironment();
}

###############################################################################
# @inherit
##
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  my @SPECIFIERS = qw(
    command
    confFile=
    debug?
    help?
    logfile=
    noRun?
    verbose?
  );

  my $command = $self->{command};
  if (($command eq "activate")
      || ($command eq "deactivate")
      || ($command eq "disableCompression")
      || ($command eq "disableDeduplication")
      || ($command eq "enableCompression")
      || ($command eq "enableDeduplication")) {
    push(@SPECIFIERS, qw(all? deviceName=--name));
  } elsif ($command eq "status") {
    push(@SPECIFIERS, qw(all? deviceName=--name pending?));
  } elsif ($command eq "create") {
    push(@SPECIFIERS, qw(activate=
			 blockMapCacheSize=
			 blockMapPeriod=
			 compression=
			 deduplication=
			 device=
			 deviceName=--name
			 emulate512=
			 force?
			 indexMem=
			 maxDiscardSize=
			 sparseIndex=
			 uuid=
			 vdoAckThreads=
			 vdoBioRotationInterval=
			 vdoBioThreads=
			 vdoCpuThreads=
			 vdoHashZoneThreads=
			 vdoLogicalSize=
			 vdoLogicalThreads=
			 vdoLogLevel=
			 vdoPhysicalThreads=
			 vdoSlabSize=));
  } elsif ($command eq "growLogical") {
    push(@SPECIFIERS, qw(deviceName=--name vdoLogicalSize=));
  } elsif ($command eq "growPhysical") {
    push(@SPECIFIERS, qw(deviceName=--name));
  } elsif ($command eq "import") {
    push(@SPECIFIERS, qw(activate=
			 blockMapCacheSize=
			 blockMapPeriod=
			 compression=
			 deduplication=
			 device=
			 deviceName=--name
			 emulate512=
			 maxDiscardSize=
			 uuid=
			 vdoAckThreads=
			 vdoBioRotationInterval=
			 vdoBioThreads=
			 vdoCpuThreads=
			 vdoHashZoneThreads=
			 vdoLogicalThreads=
			 vdoLogLevel=
			 vdoPhysicalThreads=));
  } elsif ($command eq "list") {
    push(@SPECIFIERS, qw(all?));
  } elsif ($command eq "modify") {
    push(@SPECIFIERS, qw(all?
			 blockMapCacheSize=
			 blockMapPeriod=
			 deviceName=--name
			 maxDiscardSize=
			 uuid=
			 vdoAckThreads=
			 vdoBioRotationInterval=
			 vdoBioThreads=
			 vdoCpuThreads=
			 vdoHashZoneThreads=
			 vdoLogicalThreads=
			 vdoPhysicalThreads=));
  } elsif ($command eq "printConfigFile") {
    push(@SPECIFIERS, qw());
  } elsif (($command eq "remove")
            || ($command eq "stop")) {
    push(@SPECIFIERS, qw(all? deviceName=--name force?));
  } elsif ($command eq "start") {
    push(@SPECIFIERS, qw(all? deviceName=--name forceRebuild?));
  } elsif ($command eq "version") {
    push(@SPECIFIERS, qw());
  }

  return $self->SUPER::getArguments(@SPECIFIERS);
}

1;
