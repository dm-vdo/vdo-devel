##
# Framework for scripts.
#
# @synopsis
#
# use Permabit::ScriptFramework;
#
# sub main {
# }
#
# Permabit::ScriptFramework->run(...);
#
# @description
#
# C<Permabit::ScriptFramework> provides a framework for building scripts which
# may need to back out portions of their work on errors.
##
package Permabit::ScriptFramework;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Fatal qw(
  open
  close
  rename
);

use Cwd;
use File::Path qw(make_path);
use Getopt::Long qw(GetOptions);
use IO::Dir;
use IO::File;
use Log::Log4perl qw(:easy);
use POSIX qw(strftime);

use Pdoc::Generator qw(pdoc2usage);
use Permabit::Assertions qw(assertMinArgs
                            assertMinMaxArgs
                            assertNumArgs);
use Permabit::SystemUtils qw(assertCommand
                             assertSystem);

our %PROPERTIES
  = (
     # @ple A method for parsing arguments remaining after GetOptions().
     argParser                => undef,
     # @ple Clean ups to run on exit.
     cleanups                 => [],
     # @ple A command line argument description for GetOptions().
     commandLineArguments     => undef,
     # @ple The defaults for command line options.
     config                   => {},
     # @ple An environment variable which, if set to true, puts the script
     #      in debug mode.
     debugEnvironmentVariable => undef,
     # @ple The text of any fatal error message or undef if there isn't one.
     errorText                => undef,
     # @ple Any additional features for the framework
     features                 => {},
     # @ple The log level of the script.
     logLevel                 => 'INFO',
     # @ple The logger.
     logger                   => undef,
     # @ple Whether to include timestamps in output
     logTimestamps            => 0,
     # @ple The main method for the script
     mainMethod               => \&main::main,
     # @ple The remote host on which to run commands
     remoteHost               => undef,
    );

######################################################################
# Make a new script.
#
# @param  package  The name of the package to bless into
# @oparam args     A hash of named arguments
##
sub new {
  my ($package, %args) = assertMinArgs(1, @_);
  my $self = bless { %PROPERTIES, %args }, $package;
  foreach my $feature (keys(%{$self->{features}})) {
    my $module = join('::', __PACKAGE__, $feature);
    eval("use $module");
    if ($EVAL_ERROR) {
      die($EVAL_ERROR);
    }
  }
  return $self;
}

######################################################################
# Make a new script and run it.
#
# @param  package  The name of the package to bless into
# @oparam args     A hash of named arguments
##
sub run {
  my ($package, %parameters) = assertMinArgs(3, @_);
  my $self = $package->new(%parameters);
  $self->parseCommandLine();
  $self->setUp();
  eval {
    $self->{mainMethod}->($self);
  };
  if ($EVAL_ERROR) {
    $self->{errorText} = $EVAL_ERROR;
    $self->fatal($EVAL_ERROR);
  }

  $self->cleanUp();
  if ($self->{errorText}) {
    die($self->{errorText});
  }

  exit(0);
}

######################################################################
# Parse command line arguments.
##
sub parseCommandLine {
  my ($self) = assertNumArgs(1, @_);
  if (defined($self->{commandLineArguments})) {
    if (!GetOptions($self->{config}, @{$self->{commandLineArguments}})) {
      pdoc2usage();
    }
  }

  if (defined($self->{argParser})) {
    $self->{argParser}($self);
    return;
  }

  if (scalar(@ARGV) > 0) {
    pdoc2usage();
  }
}

######################################################################
# Set up the framework.
##
sub setUp {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{debugEnvironmentVariable}
      && $ENV{$self->{debugEnvironmentVariable}}) {
    $self->set('debug', 1);
  }

  my $logLevel  = ($self->get('debug') ? 'DEBUG' : $self->{logLevel});
  my $layout    = ($self->{logTimestamps} ? '%d %m%n' : '%m%n');
  my $logConfig = << "EOCONFIG";
log4perl.rootLogger = $logLevel, Screen

# Log to STDERR
log4perl.appender.Screen        = Log::Log4perl::Appender::Screen
log4perl.appender.Screen.stderr = 0
log4perl.appender.Screen.layout = Log::Log4perl::Layout::PatternLayout
log4perl.appender.Screen.layout.ConversionPattern = $layout

# Turn down some low level modules, whose logging can be rather verbose
log4perl.category.Permabit.SSHMuxIPCSession = INFO
log4perl.category.Proc.Simple               = INFO
EOCONFIG

  Log::Log4perl->init(\$logConfig);
  $self->{logger} = Log::Log4perl->get_logger('main');
  $main::log      = $self->{logger};
}

######################################################################
# Clean up.
##
sub cleanUp {
  my ($self) = assertNumArgs(1, @_);

  my $cleanupError;
  foreach my $cleanup (@{$self->{cleanups}}) {
    if (($cleanup->{onError} && !$self->hasError())
        || ($cleanup->{notDebug} && $self->isDebug())) {
      next;
    }
    eval {
      &{$cleanup->{cleanup}}($self, $cleanup)
    };
    if ($EVAL_ERROR) {
      $cleanupError //= $EVAL_ERROR;
      $self->error("clean up error: $EVAL_ERROR");
    }
  }

  $self->{errorText} //= $cleanupError;
}

######################################################################
# Make the description of a file or directory for logging.
#
# @param path  The path
# @oparam description  The description
#
# @return The way that log messages should refere to the file or directory
##
sub _describePath {
  my ($path, $description) = assertMinMaxArgs(1, 2, @_);
  return (defined($description) ? "$description $path" : $path);
}

######################################################################
# A clean up function to delete a file or directory.
#
# @param context    The cleanup context
##
sub cleanFile {
  my ($self, $context) = assertNumArgs(2, @_);
  my $name = _describePath($context->{path}, $context->{description});
  $self->info("Removing $name");
  assertSystem("rm -rf $context->{path}");
}

######################################################################
# Check whether configuration values exist.
#
# @param keys  A list of keys
#
# @return True or false for each key depending on whether or not they
#         are defined
##
sub exists {
  my ($self, @keys) = assertMinArgs(2, @_);
  my @values = map({ exists($self->{config}{$_}) } @keys);
  return (wantarray ? @values : $values[0]);
}

######################################################################
# Get configuration values.
#
# @param keys  A list of keys
#
# @return The values of the specified keys
##
sub get {
  my ($self, @keys) = assertMinArgs(2, @_);
  my @values = map({ $self->{config}{$_} } @keys);
  return (wantarray ? @values : $values[0]);
}

######################################################################
# Set a configuration value.
#
# @param key    The key to set
# @param value  The value to set
#
# @return The value which was set
##
sub set {
  my ($self, $key, $value) = assertNumArgs(3, @_);
  return $self->{config}{$key} = $value;
}

######################################################################
# Unset a configuration value.
#
# @param key  The key to unset
##
sub unset {
  my ($self, $key) = assertNumArgs(2, @_);
  delete $self->{config}{$key};
}

######################################################################
# Check whether there has been an error.
#
# @return a true value if there has been a regular (non-cleanup) error
##
sub hasError {
  my ($self) = assertNumArgs(1, @_);
  return defined($self->{errorText});
}

######################################################################
# Check whether we are in debug mode.
#
# @return a true value if the script is in debug mode
##
sub isDebug {
  my ($self) = assertNumArgs(1, @_);
  return $self->get('debug');
}

######################################################################
# Add a clean-up.
#
# @param  cleanup  The clean up function to add
# @oparam args     A hash of named arguments which may contain:
#                    onError: if true, only clean up if there was an error
#                    notDebug: if true, don't clean up in debug mode
#                    any other context needed for the clean up function
##
sub addCleanup {
  my ($self, $cleanup, %args) = assertMinArgs(2, @_);
  $args{cleanup} = $cleanup;
  unshift(@{$self->{cleanups}}, { %args });
}

######################################################################
# Add a file or directory to be cleaned up on exit.
#
# @param  path  The path of the file or directory to clean up
# @oparam args  A hash of named arguments which may contain:
#               description: the description of the directory for logging
#               onError: if true, only clean up if there was an error
#               notDebug: if true, don't clean up in debug mode
##
sub addFileCleanup {
  my ($self, $path, %args) = assertMinArgs(2, @_);
  $self->addCleanup(\&cleanFile, path => $path, %args);
}

######################################################################
# Make a directory.
#
# @param   path  The path of the directory to make
# @oparam  args A hash of named arguments which may contain:
#               description: the description of the directory for logging
#               failIfExists: will fail if the directory already exists
#               mode: the mode of the directory, defaults to 0777
#               preDelete: will delete the directory before making it if it
#                          already exists
#               cleanup: a hashref of arguments to pass to addFileCleanup,
#                        if not supplied, the directory will not be cleaned up
#                        on exit
##
sub makeDirectory {
  my ($self, $path, %args) = assertMinArgs(2, @_);
  my $name = _describePath($path, $args{description});
  if (-d $path) {
    if ($args{preDelete}) {
      $self->debug("Pre-removing $name");
      assertSystem("rm -rf $path");
    } elsif ($args{failIfExists}) {
      die("$name already exists");
    } else {
      return;
    }
  }

  my $mode = $args{mode} // 0777;
  if (make_path($path, { mode => $mode, verbose => $self->isDebug() }) == 0) {
    die("Failed to create $name: $!");
  }

  $self->info("Created $name");

  if ($args{cleanup}) {
    if (defined($args{description})) {
      $args{cleanup}->{description} = $args{description};
    }
    $self->addFileCleanup($path, %{$args{cleanup}});
  }
}

######################################################################
# Change to a directory.
#
# @param   path          The path of the directory to cd into
# @oparam  description   The description of the directory for logging
##
sub cd {
  my ($self, $path, $description) = assertMinMaxArgs(2, 3, @_);
  if (chdir($path) != 0) {
    return;
  }

  my $name = _describePath($path, $description);
  die("Failed to cd to $name: $!");
}

######################################################################
# Return to the CWD after executing a sub.
#
# @param code  The sub to run
#
# @return The return from the sub
##
sub resetCWD {
  my ($self, $code) = assertNumArgs(2, @_);
  my $dir = cwd();
  my @result;
  eval {
    @result = &$code();
  };

  my $error = $EVAL_ERROR;
  $self->cd($dir);
  if ($error) {
    die($error);
  }

  return @result;
}

######################################################################
# Open a handle.
#
# @param  path         The path to open
# @oparam description  The description of the path for logging
##
sub openHandle {
  my ($self, $path, $description) = assertMinMaxArgs(2, 3, @_);
  my $fh = IO::File->new($path);
  if (defined($fh)) {
    return $fh;
  }

  die('Failed to open ' . _describePath($path, $description));
}

######################################################################
# Create a file and return a handle to it.
#
# @param  path  The path to the file
# @oparam args  A hash of named arguments which may contain:
#                 description: the description of the file for logging
#                 failIfExists: will fail if the file already exists
#                 mode: the mode of the file, defaults to 0666
#                 noLog: if true, don't log the created message
#                 cleanup: a hashref of arguments to pass to addFileCleanup,
#                          if not supplied, the file will not be cleaned
#                          up on exit
##
sub createFile {
  my ($self, $path, %args) = assertMinArgs(2, @_);
  my $name = _describePath($path, $args{description});
  if ($args{failIfExists} && (-e $path)) {
    die("$name already exists");
  }

  my $mode = $args{mode} // 0666;
  my $fh = IO::File->new(">$path");
  if (!defined($fh)) {
    die("Failed to create $name: $!");
  }

  if (chmod($mode, $path) != 1) {
    $fh->close();
    die("Failed to chmod $name: $!");
  }

  if (!$args{noLog}) {
    $self->info("Created $name");
  }

  if ($args{cleanup}) {
    if (defined($args{description})) {
      $args{cleanup}->{description} = $args{description};
    }
    $self->addFileCleanup($path, %{$args{cleanup}});
  }

  return $fh;
}

######################################################################
# Read a file and return its contents.
#
# @param  path         The path of the file to read
# @oparam description  The description of the path for logging
#
# @return The contents of the file as an array of lines in array context,
#         or a string in scalar context
##
sub readFile {
  my ($self, $path, $description) = assertMinMaxArgs(2, 3, @_);
  my $fh = $self->openHandle($path, $description);
  my @contents = $fh->getlines();
  $fh->close();
  return (wantarray() ? @contents : join('', @contents));
}

######################################################################
# Write a file.
#
# @param  path      The path of the file to write
# @param  contents  The contents of the file (to be written)
# @oparam args      A hash of named arguments which may contain:
#                     description: the description of the file for logging
#                     failIfExists: will fail if the file already exists
#                     mode: the mode of the file, defaults to 0666
#                     noLog: if true, don't log the created message
#                     cleanup: a hashref of arguments to pass to
#                       addFileCleanup, if not supplied, the file will not be
#                       cleaned up on exit
#
##
sub writeFile {
  my ($self, $path, $contents, %args) = assertMinArgs(3, @_);
  my $fh = $self->createFile($path, %args);
  $fh->print($contents);
  $fh->close();
}

######################################################################
# Read directory and return its contents, stripping out '.' and '..'.
#
# @param path  The path of the file to read
#
# @return The directory contents as an array
##
sub readDirectory {
  my ($self, $path) = assertNumArgs(2, @_);
  my $dir = IO::Dir->new($path);
  if (!defined($dir)) {
    die("Failed to open directory $path\n");
  }

  my @entries = grep(!/^\.\.?\z/, $dir->read());
  $dir->close();
  return @entries;
}

######################################################################
# Run a local command if this isn't a dry run, otherwise just log the
# command that would be run.
#
# @param command  The command to run
#
# @return The output from the command
##
sub runLocalForReal {
  my ($self, $command) = assertNumArgs(2, @_);

  if ($self->get('dryRun')) {
    $self->debug("would run locally: $command");
    return undef;
  }

  return assertSystem($command)->{stdout};
}

######################################################################
# Run a remote command.
#
# @param command  The command to run
#
# @return The output from the command
##
sub runRemote {
  my ($self, $command) = assertNumArgs(2, @_);

  return assertCommand($self->get('remoteHost'), $command)->{stdout}
}

######################################################################
# Run a remote command if this isn't a dry run, otherwise just log the
# command that would be run.
#
# @param command  The command to run
#
# @return The output from the command
##
sub runRemoteForReal {
  my ($self, $command) = assertNumArgs(2, @_);

  if ($self->get('dryRun')) {
    $self->debug(":would run remotely: $command");
    return undef;
  }

  return $self->runRemote($command);
}

######################################################################
# Log a debug message.
#
# @param message  The message to log
##
sub debug {
  my ($self, $message) = assertNumArgs(2, @_);
  $self->{logger}->debug($message);
}

######################################################################
# Log an info message.
#
# @param message  The message to log
##
sub info {
  my ($self, $message) = assertNumArgs(2, @_);
  $self->{logger}->info($message);
}

######################################################################
# Log a warning message.
#
# @param message  The message to log
##
sub warn {
  my ($self, $message) = assertNumArgs(2, @_);
  $self->{logger}->warn($message);
}

######################################################################
# Log an error message.
#
# @param message  The message to log
##
sub error {
  my ($self, $message) = assertNumArgs(2, @_);
  $self->{logger}->error($message);
}

######################################################################
# Log a fatal message.
#
# @param message  The message to log
##
sub fatal {
  my ($self, $message) = assertNumArgs(2, @_);
  $self->{logger}->fatal($message);
}

1;
