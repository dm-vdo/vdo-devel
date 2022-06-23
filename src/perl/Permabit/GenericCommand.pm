######################################################################
# An instance of an generic command.
#
# OVERVIEW
#
#  There is a lot of setup done around pretty much every command run in the
#  test infrastructure. This class encapsulates that setup and isolates it
#  from the test runner.
#
# CONSTRUCTION
#
#  Each GenericCommand declares the properties it requires, and these
#  properties are one of two kinds: regular properties, and "inherited"
#  properties. These are defined by getProperties() and
#  getInheritedProperties().
#
#   getParameters() - Returns the regular properties of a GenericCommand, and
#     their defaults.
#
#   getProperties() - Returns the regular properties of a GenericCommand, and
#     their defaults. The default constructor uses these properties as
#     constructor arguments.
#
#   getInheritedProperties() - Returns the properties which may be inherited
#     from the testcase, and their default values if not otherwise provided.
#
#   new() - The base-class constructor declared here is typically
#     sufficient for most subclasses, and does not necessarily need to be
#     redefined.  This constructor integrates properties from the testcase
#     and the additional constructor arguments, with their default values
#     in getProperties() and getInheritedProperties().
#
#  Occasionally, properties from an object other than the testcase should
#  be "inherited" by the GenericCommand.  If necessary, after construction,
#  addInheritedProperties() may be used to copy these properties into the
#  testcase.
#
# COMMAND RESOLUTION
#
#  Commands typically declare a "name" property, which is the base name of
#  the command encapsulated by the subclass of GenericCommand.  At
#  construction time, if a property called "binary" isn't found, then this
#  name is looked for using the BinaryFinder which is shared with the
#  testcase.
#
# COMMAND CONSTRUCTION AND RUNNING
#
#  GenericCommand declares a method run() which runs the command.  If the
#  property "allowFailure" is true, the command is permitted to fail.
#
#  The command is contructed via buildCommand() and stored in "_command".
#  buildCommand() itself delegates to 4 additional functions, which contruct
#  various pieces of the command.  In order, they are:
#
#   buildBaseCommand() - Typically does not need to be overridden.  This command
#     component simply changes the working directory to runDir (inherited from
#     the testcase), and sets the umask and ulimits.
#
#   buildEnvironment() - Using the hash variable at $self->{env}, this sets the
#     environment variable for the command. Subclasses may add additional
#     variables to the environment.
#
#   buildMainCommand() - Constructs the command which will run under the
#     environment provided by buildEnvironment.
#
#   getArguments() - Constructs the command line arguments for the command. It
#     typically uses the convenience functions _addSimpleOption() and
#     _addValueOption().
#
#   getRedirects() - Adds any output redirection for the command.
#
# $Id$
##
package Permabit::GenericCommand;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Carp;
use Log::Log4perl;
use Storable qw(dclone);

use Permabit::Assertions qw(
                             assertDefined
                             assertMinMaxArgs
                             assertNotDefined
                             assertNumArgs
                          );
use Permabit::SystemUtils qw(assertCommand runCommand);
use Permabit::Utils qw(hashExtractor mergeToHash);

use base qw(Permabit::BinaryFinder);

# Overload stringification to print something meaningful
use overload q("") => \&as_string;

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %PROPERTIES =
  (
   # Whether to allow failures without croaking
   allowFailure    => 0,
   # The binary to use
   binary          => undef,
   # The command to run
   _command        => undef,
   # Any CPU affinity settings to impose (list)
   cpuAffinityList => undef,
   # Any CPU affinity settings to impose (mask)
   cpuAffinityMask => undef,
   # Whether to use sudo or not
   doSudo          => 0,
   # Whether to run the command as a particular user
   doUser          => undef,
   # Environmental variables to set inside _command
   env             => {},
   # Extra arguments to append to the command's argument list
   extraArgs       => undef,
   # The host to run on
   host            => undef,
   # Executable name (required by constructor)
   name            => undef,
   # Whether the command is currently running
   _running        => 0,
   # Whether the consumer of this GenericCommand is not a BinaryFinder, having
   # its own binary field defined.
   standAlone      => 0,
   # If defined, a timeout to be applied to the command
   timeout         => undef,
  );

my %INHERITED_PROPERTIES =
  (
   # @ple Process environment hash
   environment => { },
   # Run directory of commands
   runDir      => undef,
   # The directory to put temp files in
   workDir     => undef,
  );

#############################################################################
# Get the properties for this command.
##
sub getProperties {
  my ($self) = assertNumArgs(1, @_);
  return %PROPERTIES;
}

#############################################################################
# Get the parameters for this command.
##
sub getParameters {
  my ($self) = assertNumArgs(1, @_);
  return [ keys(%PROPERTIES) ];
}

#############################################################################
# Get the host on which the command is run.
##
sub getHost {
  my ($self) = assertNumArgs(1, @_);
  return $self->{host};
}

#############################################################################
# Get the set of properties that may be inherited from the testcase
##
sub getInheritedProperties {
  my ($self) = assertNumArgs(1, @_);
  return %INHERITED_PROPERTIES;
}

######################################################################
# Instantiates a new GenericCommand.
#
# @param parent     The parent from which inherited properties will be
#                   culled.  This can be either an AlbireoTest, or another
#                   Permabit::GenericCommand.
# @param arguments  Additional constructor arguments
##
sub new {
  my ($invocant, $parent, $arguments) = assertNumArgs(3, @_);
  my $class = ref($invocant) || $invocant;

  $log->debug("Creating instance of $class");
  if (defined($arguments->{options})) {
    $arguments = {%$arguments, %{$arguments->{options}}};
  }

  my %inherited = $class->getInheritedProperties();
  my %args = ( hashExtractor($parent, [keys %inherited]),
               %{$arguments});

  if (defined($args{environment})) {
    mergeToHash(\%args, (env => $args{environment}));
  }
  my %defaults = %{dclone({$class->getProperties(),
                           $class->getInheritedProperties()})};

  for my $k (keys %defaults) {
    if (!defined($args{$k})) {
      $args{$k} = $defaults{$k};
    }
  }

  my $self = bless(\%args, $class);

  # If the "parent" of this object is not a BinaryFinder, it must have its
  #  own binary defined
  # XXX instead of a standAlone field, it would be better if there was
  #     a "StandAlone" extension of the BinaryFinder objects.
  if (! $parent->{standAlone}) {
    $self->shareBinaryFinder($parent);
    if (! defined($self->{binary})) {
      $self->updateBinary();
    }
  }

  assertDefined($self->{binary}, "binary must be defined");

# XXX We couldn't assert that host is defined because AlbireoIndexer runs on
#     multiple (or zero!) machines and doesn't use host.  AlbireoIndexer no
#     longer exists, and it may be possible to assert this now.
# TODO: make this more sane.
#   assertDefined($self->{host}, "host must be defined");

  return $self;
}

######################################################################
# Update the binary path based on the name (or other factors
# subclasses might care about).
##
sub updateBinary {
  my ($self) = assertNumArgs(1, @_);
  assertDefined($self->{name}, "Command has no name!");
  $self->{binary} = $self->findBinary($self->{name});
}

######################################################################
# Add inherited properties from the provided hash.
#
# @param propSource  The hash from which to pull properties
##
sub addInheritedProperties {
  my ($self, $propSource) = assertNumArgs(2, @_);
  my %inherited = $self->getInheritedProperties();
  for my $k (keys %inherited) {
    if ($propSource->{$k}) {
      $self->{$k} = $propSource->{$k};
    }
  }
}

#############################################################################
# Determines if the given flag is an old style short option (single -).
#
# @param flag  The command-line option to check
##
sub isShortOption {
  my ($flag) = assertNumArgs(1, @_);
  return scalar($flag =~ /^-[^-]/); # first char is a '-', the second char can
}                                   # be anything but.

#############################################################################
# Add a simple option to the argument list if necessary
#
# @param argsRef   A reference to the list of arguments
# @param property  The property of $self to check
# @param flag      The command-line switch to add
##
sub _addSimpleOption {
  my ($self, $argsRef, $property, $flag) = assertNumArgs(4, @_);
  if ($self->{$property}) {
    push(@$argsRef, $flag);
  }
}

#############################################################################
# Add an option with a value to the argument list, if defined. For GNU long
# style options, a '=' will be placed between the flag and the property,
# otherwise nothing will seperate the flag from the property.
#
# @param argsRef   A reference to the list of arguments
# @param property  The property of $self to check
# @param flag      The command-line switch to add
##
sub _addValueOption {
  my ($self, $argsRef, $property, $flag) = assertNumArgs(4, @_);
  if (defined($self->{$property})) {
    push(@$argsRef,
         $flag . (isShortOption($flag) ? ' ' : '=')
               . $self->{$property});
  }
}

######################################################################
# Check if the command is currently running.
#
# @return  <code>true</code> if the command is currently running
##
sub isRunning {
  my ($self) = assertNumArgs(1, @_);
  return $self->{_running};
}

######################################################################
# Build a list of arguments for the command.
#
# @return  The list of arguments
##
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  if (!defined($self->{extraArgs})) {
    return ();
  }

  return ( $self->{extraArgs} );
}

######################################################################
# Return any output redirection for the command, as a string.
#
# @return  The output redirection string, if any
##
sub getRedirects {
  my ($self) = assertNumArgs(1, @_);
  return ();
}

######################################################################
# Build the start of every command. By default this configures commands
# to run out of runDir, sets the umask to be permissive and makes sure
# we get core files no matter how big they are. Most of our test
# infrastructure depends on this behavior.
#
# @return  A list of command string fragments; should not be empty.
##
sub buildBaseCommand {
  my ($self) = assertNumArgs(1, @_);
  my @cmd = ("cd $self->{runDir} && umask 0 && ulimit -c unlimited &&");
  return @cmd;
}

######################################################################
# Create the command(s) that wrap the "main" command.
#
# @return  a list of wrapper commands
##
sub buildWrapper {
  my ($self) = assertNumArgs(1, @_);
  my @cmd;

  if ($self->{cpuAffinityMask}) {
    assertNotDefined($self->{cpuAffinityList},
                     "cpuAffinityMask and cpuAffinityList"
                     . " may not both be defined");
    push(@cmd, "taskset", $self->{cpuAffinityMask});
  } elsif ($self->{cpuAffinityList}) {
    push(@cmd, "taskset -c", $self->{cpuAffinityList});
  }

  if ($self->{doSudo}) {
    push(@cmd, "sudo");
    if (defined $self->{doUser}) {
      push(@cmd, "-u", $self->{doUser});
    }
  }

  if (defined($self->{timeout})) {
    my $timeout = int($self->{timeout});
    if ($timeout > 0) {
      push(@cmd, "timeout", $timeout);
    }
  }

  return @cmd;
}

######################################################################
# Create the "main" command.  The "main" command is the part of the
# command to which an environment will be prefixed. By default, it's
# simply $self->{binary} unless doSudo is enabled and then 'sudo'
# is prefixed.
#
# @return  A command string fragment; an empty string should not be returned
##
sub buildMainCommand {
  my ($self) = assertNumArgs(1, @_);
  return $self->{binary};
}

######################################################################
# Augment the environment ($self->{env}) with vars needed for this command
#
# @return  A command string fragment that will setup the necessary environment
#          variables; an empty string should not be returned
##
sub buildEnvironment {
  my ($self) = assertNumArgs(1, @_);
  if (%{$self->{env}}) {
    return "env " . join(" ", map {"$_=$self->{env}->{$_}"}
                         sort keys %{$self->{env}});
  }
  return;
}

######################################################################
# Build the entire command string correctly for the command. This method
# joins together the output of buildBaseCommand, buildEnvironment,
# buildMainCommand, and getArguments with a single space.
#
# @return  The command string
##
sub buildCommand {
  my ($self) = assertNumArgs(1, @_);
  $self->{_command} = join(" ",
                           $self->buildBaseCommand(),
                           $self->buildEnvironment(),
                           $self->buildWrapper(),
                           $self->buildMainCommand(),
                           $self->getArguments(),
                           $self->getRedirects());
  return $self->{_command};
}

#############################################################################
# Run the command.
#
# @return  The result of running the command
##
sub run {
  my ($self) = assertNumArgs(1, @_);
  if (!defined($self->{_command})) {
    $self->buildCommand();
  }

  $self->{_running} = 1;
  my $result;
  if ($self->{allowFailure}) {
    $result = runCommand($self->{host}, $self->{_command});
  } else {
    $result = assertCommand($self->{host}, $self->{_command});
  }
  $self->{_running} = 0;

  return $result;
}

##########################################################################
# Overloads default stringification to print something useful.
##
sub as_string {
  my $self = shift;
  return "(" . __PACKAGE__ . ": $self->{name})";
}

1;
