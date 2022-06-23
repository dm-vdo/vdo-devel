######################################################################
# An instance of an Albireo process.
#
# This class encapsulates the necessary command line magic to use use
# profiling tools with a command.
#
# $Id$
##
package Permabit::AlbireoProfilingCommand;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use File::Basename;
use Log::Log4perl;
use Permabit::AlbireoTestUtils qw(getIndexDevice);
use Permabit::Assertions qw(assertFalse assertNumArgs);
use Permabit::SystemUtils qw(assertCommand assertScp runCommand);
use Permabit::Utils qw(makeFullPath);
use Regexp::Common;
use Statistics::Descriptive;

use base qw(Permabit::AlbireoCommand);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

# These are the properties inherited by the testcase.  Note that testcase base
# classes like AlbireoTest directly copy this hash into its own properties.
# The defaults here are then used.
our %INHERITED_PROPERTIES =
  (
   # @ple Run blktrace
   blktrace       => 0,
   # @ple use callgrind
   callgrind      => 0,
   # @ple run with coverage
   coverage       => 0,
   # @ple use drd
   drd            => 0,
   # @ple use helgrind
   helgrind       => 0,
   # @ple Limit to some number of cores
   limitCores     => undef,
   # @ple run under mutrace
   mutrace        => 0,
   # @ple Run with strace
   strace         => 0,
   # @ple run the scanner binary with valgrind
   valgrind       => 0,
   # @ple the valgrind binary
   valgrindBinary => "valgrind",
   # @ple run the scanner binary with valgrind
   valgrindMassif => 0,
   # warnings from valgrind to suppress
   valgrindSups   => undef,
  );

#############################################################################
# @inherit
##
sub getInheritedProperties {
  my $self = shift;
  return ( $self->SUPER::getInheritedProperties(),
           %INHERITED_PROPERTIES );
}

######################################################################
# @inherit
##
sub run {
  my ($self) = assertNumArgs(1, @_);
  if (!defined($self->{_command})) {
    $self->buildCommand();
  }

  if ($self->{blktrace} && $self->{indexer}) {
    $self->_startBlktrace($self->{host}, $self->{indexer});
  }

  return $self->_assertCommandAndCleanup($self->{host}, $self->{_command});
}

#############################################################################
# Build the valgrind command wrapper.
#
# @return       a command and argument list
##
sub _buildValgrindCommand {
  my ($self) = assertNumArgs(1, @_);

  my @valgrindCommand;
  if ($self->{valgrind}) {
    @valgrindCommand = ("--leak-check=full",
                        "--gen-suppressions=all");
    if ($self->{valgrindSups}) {
      push(@valgrindCommand, "--suppressions=$self->{valgrindSups}");
    }
  } elsif ($self->{valgrindMassif}) {
    push(@valgrindCommand, "--tool=massif");
  } elsif ($self->{helgrind}) {
    push(@valgrindCommand, "--tool=helgrind");
  } elsif ($self->{callgrind}) {
    push(@valgrindCommand, "--tool=callgrind");
  } elsif ($self->{drd}) {
    push(@valgrindCommand, "--tool=drd");
  }
  if (@valgrindCommand) {
    unshift(@valgrindCommand, $self->{valgrindBinary});
    if (!$self->{valgrindMassif}) {
      push(@valgrindCommand, "--log-file=$self->{_valgrindFile}");
    }
  }
  return @valgrindCommand;
}

######################################################################
# Are we using valgrind?
##
sub shouldUseValgrind {
  my ($self) = assertNumArgs(1, @_);
  return ($self->{valgrind} || $self->{callgrind}
          || $self->{drd} || $self->{helgrind} || $self->{valgrindMassif});
}

######################################################################
# @inherit
##
sub buildEnvironment {
  my ($self) = assertNumArgs(1, @_);
  if ($self->shouldUseValgrind()) {
    $self->{env}->{GLIBCPP_FORCE_NEW} = 1;
  }
  if ($self->{coverage}) {
    $self->{env}->{GCOV_PREFIX}       = $self->{runDir};
    $self->{env}->{GCOV_PREFIX_STRIP} = 10;
  }
  return $self->SUPER::buildEnvironment();
}

######################################################################
# @inherit
##
sub buildBaseCommand {
  my ($self) = assertNumArgs(1, @_);
  my @command = $self->SUPER::buildBaseCommand();
  if ($self->shouldUseValgrind()) {
    $self->{_valgrindFile} = makeFullPath($self->{runDir}, 'valgrind.out');
    push(@command, "touch $self->{_valgrindFile} &&");
  }
  return @command;
}

######################################################################
# @inherit
##
sub buildWrapper {
  my ($self) = assertNumArgs(1, @_);
  my @command;
  if ($self->{limitCores}) {
    push(@command, "taskset -c 0-" . ($self->{limitCores} - 1));

  }
  if ($self->{strace}) {
    push(@command, "strace -o", makeFullPath($self->{runDir}, "strace.out"),
         "-f -e 'close,read,pread,lseek,write,pwrite,fsync,open' -T -r");
  }
  if ($self->{mutrace}) {
    push(@command, "mutrace --debug-info");
  }
  if ($self->shouldUseValgrind()) {
    push(@command, $self->_buildValgrindCommand());
  }
  return ($self->SUPER::buildWrapper(), @command);
}

######################################################################
# @inherit
##
sub getLogfileName {
  my ($self) = assertNumArgs(1, @_);
  return makeFullPath($self->{runDir}, $self->SUPER::getLogfileName());
}

#############################################################################
# Check the valgrind result for a command
#
# @param host           the host the command ran on
# @param file           the output file from valgrind
#
# @croaks               if valgrind indicates any errors
##
sub _checkValgrind {
  my ($self, $host, $valgrindFile) = assertNumArgs(3, @_);

  my $failure = 0;

  my $result = assertCommand($host, "cat $valgrindFile");

  while ($result->{stdout} =~ /ERROR SUMMARY: (\d+) errors/g) {
    my $errorCount = $1;
    if ($errorCount > 0) {
      ++$failure;
    }
  }

  while ($result->{stdout} =~ /lost: ([\d,]+) bytes/g) {
    my $bytes = $1;
    $bytes =~ s/,//g;           # remove thousands-separating commas
    if ($bytes > 0) {
      ++$failure;
    }
  }
  assertFalse($failure, "valgrind failure");
}

#############################################################################
# Runs the given command and performs final checks on the result.
#
# @param client        the client on which to run the command
# @param command       the command to run
#
# @return              results from running the command
##
sub _assertCommandAndCleanup {
  my ($self, $client, $command) = assertNumArgs(3, @_);

  my $result;
  eval {
    if ($self->{allowFailure}) {
      $result = runCommand($client, $command);
    } else {
      $result = assertCommand($client, $command);
    }
    if ($self->{valgrind} || $self->{helgrind} || $self->{drd}) {
      $self->_checkValgrind($client, $self->{_valgrindFile});
    }
  };
  my $error = $EVAL_ERROR;
  if ($self->{blktrace} && $self->{indexer}) {
    $self->_stopBlktrace($client, $self->{indexer});
  }
  if ($error) {
    # Rethrow the error caught by eval - the original thrower has already
    # decorated the error by using croak or confess, so there is no need
    # for us to add to it
    die($error);
  }
  if ($self->{strace} && $self->{_straceFile}) {
    $self->_summarizeTrace($client);
  }
  return $result;
}

#############################################################################
# Start blktrace.
#
# @param client        the client on which to run albscan
# @param indexer       the indexer process hash
##
sub _startBlktrace {
  my ($self, $client, $indexer) = assertNumArgs(3, @_);
  my $dev = getIndexDevice($client, $indexer->{indexDir});
  $self->{_blktraceFile} = makeFullPath($self->{runDir}, 'blktrace');
  assertCommand($client,
                "sudo blktrace -d $dev -D $self->{runDir} -o blktrace &");
}

#############################################################################
# Stop blktrace.
#
# @param client        the client on which to run albscan
# @param indexer       the indexer process hash
##
sub _stopBlktrace {
  my ($self, $client, $indexer) = assertNumArgs(3, @_);
  my $dev = getIndexDevice($client, $indexer->{indexDir});
  assertCommand($client, "sudo blktrace -d $dev -k");
}

#############################################################################
# Summarize the strace file.
#
# @param client         the host it ran on
##
sub _summarizeTrace {
  my ($self, $client) = assertNumArgs(2, @_);

  my $name = basename($self->{_straceFile});
  my $localFile = makeFullPath($self->{workDir}, $name);
  assertScp("$client:$self->{_straceFile}", $localFile);
  open(my $fh, "<$localFile");
  my %stats;
  while (<$fh>) {
    chomp;
    if (/^$RE{num}{int}\s+$RE{num}{real}\s+(\w+).*<($RE{num}{real})>$/) {
      my ($call, $time) = ($1, $2);
      if (!exists($stats{$call})) {
        $stats{$call} = Statistics::Descriptive::Sparse->new();
      }
      $stats{$call}->add_data($time);
    }
  }
  close($fh);
  $log->info(sprintf("%10s %10s %10s/%10s/%10s/%10s",
                     "call", "count", "min", "mean", "max", "std-dev"));
  foreach my $k (keys %stats) {
    my $s = $stats{$k};
    $log->info(sprintf("%-10s %10d %10f/%10f/%10f/%10f",
                       $k, $s->count(), $s->min(), $s->mean(), $s->max(),
                       $s->standard_deviation()));
  }
}

##########################################################################
# Overloads default stringification to print our service and hostname.
##
sub as_string {
  my $self = shift;
  return "(AlbireoCommand on $self->{host})";
}

1;
