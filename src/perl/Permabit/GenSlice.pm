##
# Perl object that encapsulates a slice of a block device populated with
# data using genDataBlocks.
#
# @synopsis
#
#   # Generate and verify a dataset on an entire device
#   use Permabit::Assertions qw(assertType);
#   use Permabit::GenSlice;
#   assertType("Permabit::BlockDevice", $device);  # precondition
#   my $slice = Permabit::GenSlice->new(device => $device);
#   $slice->write(tag => "entire");
#   $slice->verify();
#
#   # Generate and verify a dataset on 5000 blocks, with dedupe
#   use Permabit::Assertions qw(assertType);
#   use Permabit::GenSlice;
#   assertType("Permabit::BlockDevice", $device);  # precondition
#   my $slice = Permabit::GenSlice->new(device     => $device,
#                                       blockCount => 5000);
#   $slice->write(tag => "half", dedupe => 0.5);
#   $slice->verify();
#
#   # Generate and verify a dataset on 5000 blocks, with compression
#   use Permabit::Assertions qw(assertType);
#   use Permabit::GenSlice;
#   assertType("Permabit::BlockDevice", $device);  # precondition
#   my $slice = Permabit::GenSlice->new(device     => $device,
#                                       blockCount => 5000);
#   $slice->write(tag => "squish", compress => 0.6);
#   $slice->verify();
#
#   # Generate data in an AsyncSub, and verify the data in the main process
#   use Permabit::Assertions qw(assertType);
#   use Permabit::GenSlice;
#   assertType("Permabit::BlockDevice", $device);  # precondition
#   my $slice = Permabit::GenSlice->new(device => $device);
#   my $sub = sub {
#                  $slice->write(tag => "data");
#                  return $slice->exportData();
#                 };
#   my $task = Permabit::AsyncSub->new(code => $sub);
#   $task->start();
#   $slice->importData($task->result());
#   $slice->verify();
#
# $Id$
##
package Permabit::GenSlice;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Basename;
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertMinArgs
  assertNotDefined
  assertNumArgs
  assertRegexpMatches
  assertTrue
  assertType
);
use Permabit::Constants;
use Permabit::Exception qw(Verify);
use Permabit::PlatformUtils qw(isMaipo);
use Permabit::Utils qw(makeFullPath rethrowException);
use Storable qw(dclone);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{new}
my %PROPERTIES
  = (
     # @ple The number of blocks per file or slice
     blockCount       => undef,
     # @ple The number of bytes in a block
     blockSize        => undef,
     # @ple The data written by genDataBlocks to the slice
     data             => {},
     # @ple The Permabit::BlockDevice that the slice lives on
     device           => undef,
     # @ple The Permabit::Filesystem to write files to
     fs               => undef,
     # @ple The number of files in the slice
     fileCount        => 1,
     # @ple The block number of the first block in the slice
     offset           => 0,
     # @ple Flag set to true if only a single verify should be performed
     singlePassVerify => 0,
     # @ple The maximum number of bytes to use for files
     totalBytes       => undef,
     # @ple Flag set to true if the unwritten blocks should be zero
     zero             => 1,
    );
##

#############################################################################
# Create a C<Permabit::GenSlice>.
#
# @params{new}
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self = bless { %{dclone(\%PROPERTIES)},
                     # Overrides previous values
                     @_,
                   }, $class;

  if ($self->{fs}) {
    # Set blockCount to blocks per file
    $self->{blockSize} //= $self->{fs}->{blockSize} // 4 * $KB;
    my $totalBlocks = ($self->{totalBytes}
                       ? int($self->{totalBytes} / $self->{blockSize})
                       : $self->{blockCount});
    $self->{blockCount} = int($totalBlocks / $self->{fileCount});
    $self->{device} //= $self->{fs}->{device};
    $self->{zero} = 0;
    return $self;
  }

  assertDefined($self->{device});
  assertType("Permabit::BlockDevice", $self->{device});

  # Fill in blockCount and blockSize
  if (!defined($self->{blockCount}) || !defined($self->{blockSize})) {
    my $vdoType = 'Permabit::BlockDevice::VDO';
    my ($vdo) = ($self->{device}->isa($vdoType)
                 ? $self->{device}
                 : $self->{device}->getAncestorsOfType($vdoType));
    if (defined($vdo)) {
      my $stats = $vdo->getVDOStats();
      if (defined($stats)) {
        $self->{blockCount} //= $stats->{"logical blocks"};
        $self->{blockSize}  //= $stats->{"block size"};
      }
    }
  }

  $self->{blockSize}  //= 4 * $KB;
  $self->{blockCount} //= $self->{device}->getSize() / $self->{blockSize};
  return $self;
}

#############################################################################
# Export a description of the data in the slice, which can be passed to
# importData.
#
# @return a  hashref containing the descripttion.
##
sub exportData {
  my ($self) = assertNumArgs(1, @_);
  return { data => $self->{data}, zero => $self->{zero}, };
}

#############################################################################
# Get the device containing the slice
#
# @return the device
##
sub getDevice {
  my ($self) = assertNumArgs(1, @_);
  return $self->{device};
}

#############################################################################
# Get the file system containing the slice
#
# @return the file system
##
sub getFileSystem {
  my ($self) = assertNumArgs(1, @_);
  return $self->{fs};
}

#############################################################################
# Import a description of the data in the slice.
#
# @param import  hashref produced by exportData
##
sub importData {
  my ($self, $import) = assertNumArgs(2, @_);
  $self->{data} = $import->{data};
  $self->{zero} = $import->{zero};
}

#############################################################################
# Trim (zero) the slice.
##
sub trim {
  my ($self) = assertNumArgs(1, @_);
  assertNotDefined($self->{fs}, "trim not supported with files");
  my $machine = $self->{device}->getMachine();
  $machine->genDiscard(
                       of    => $self->{device}->getSymbolicPath(),
                       count => $self->{blockCount},
                       bs    => $self->{blockSize},
                       seek  => $self->{offset},
                      );
  $self->{data} = {};
  $self->{zero} = 1;
}

#############################################################################
# Trim (zero) the slice, allowing the trim to be interrupted by an EIO error.
##
sub trimEIO {
  my ($self) = assertNumArgs(1, @_);
  eval {
    $self->trim();
  };
  if (my $error = $EVAL_ERROR) {
    assertRegexpMatches(qr(: Input/output error), $error);
  }
}

#############################################################################
# Verify the data written to the slice.  Arguments are passed in name-value
# pairs.
#
# @oparam direct  Use O_DIRECT on the device
# @oparam sync    Use O_SYNC on the device
##
sub verify {
  my ($self, %args) = assertMinArgs(1, @_);
  assertTrue($self->{zero} || (scalar(keys(%{$self->{data}})) > 0),
             "no data to verify");
  my @options = ("--blockSize=$self->{blockSize}",
                 "--blockCount=$self->{blockCount}");
  if ($args{direct}) {
    push(@options, "--direct");
  }
  if ($args{sync}) {
    push(@options, "--sync");
  }

  my $action;
  if ($self->{fs}) {
    assertEqualNumeric(1, scalar(keys(%{$self->{data}})),
                       "multiple streams not permitted for verifyFiles");
    my ($tag, $v) = each(%{$self->{data}});
    push(@options,
         "--fileCount=$self->{fileCount}",
         "--dir=" . makeFullPath($self->{fs}->getMountDir(), $tag),
         "--data=$tag,$v->{dedupe},$v->{compress}");
    $action = "verifyFiles";
  } else {
    push(@options, "--device=" . $self->{device}->getSymbolicPath());
    while (my ($tag, $v) = each(%{$self->{data}})) {
      push(@options, "--data=$tag,$v->{dedupe},$v->{compress}");
    }
    if ($self->{offset} != 0) {
      push(@options, "--offset=$self->{offset}");
    }
    if ($self->{zero}) {
      push(@options, "--zero");
    }
    $action = "verifySlice";
  }

  eval {
    my $results = $self->_genDataBlocks($action, \@options);
    $self->_assertSuccess();

    $self->{zero} = exists($results->{ZERO}) ? 1 : 0;
    foreach my $k (keys(%{$self->{data}})) {
      if (!exists($results->{$k})) {
        delete $self->{data}{$k};
      }
    }
  };
  if (my $firstError = $EVAL_ERROR) {
    rethrowException($firstError, "Permabit::Exception::SSH");
    rethrowException($firstError, "Permabit::Exception::Signal");
    if ($self->{singlePassVerify}) {
      my $error = "Single verify error - $firstError";
      die(Permabit::Exception::Verify->new($error));
    }
    $log->fatal("First verify error: $firstError");
    # Do a verify again just in case it was a transient failure...
    my $secondResult = "second verify succeeded";
    eval {
      $self->_genDataBlocks($action, \@options);
      $self->_assertSuccess();
    };
    if (my $secondError = $EVAL_ERROR) {
      $log->fatal("Second verify error: $secondError");
      $secondResult = "second verify failed";
    }
    my $error = "First verify failed, $secondResult - $firstError";
    die(Permabit::Exception::Verify->new($error));
  }
}

#############################################################################
# Verify the data written to the slice, and then trim the slice.  Arguments are
# passed in name-value pairs, and are forwarded on to the verify() method.
#
# @oparam args  Passed onward to verify.
##
sub verifyAndTrim {
  my ($self, @args) = assertMinArgs(1, @_);
  $self->verify(@args);
  $self->trim();
}

#############################################################################
# Write the slice using genDataBlocks, expecting that the write will
# succeed.
#
# @param args  Arguments (see _writeDataBlock)
##
sub write {
  my ($self, %args) = assertMinArgs(3, @_);
  $self->_writeDataBlocks(%args);
  $self->_assertSuccess();
}

#############################################################################
# Write the slice using genDataBlocks, expecting that the write may get an
# EIO error.  Arguments are passed in name-value pairs.
#
# @param args  Arguments (see _writeDataBlock)
##
sub writeEIO {
  my ($self, %args) = assertMinArgs(3, @_);
  $self->_writeDataBlocks(%args);
  if ($self->{_status} == 3) {
    my $machine = $self->{device}->getMachine();
    assertRegexpMatches(qr(: Input/output error), $machine->getStderr());
  } else {
    $self->_assertSuccess();
  }
}

#############################################################################
# Write the slice using genDataBlocks, expecting that the write may get an
# ENOSPC error.  Arguments are passed in name-value pairs.
#
# @param args  Arguments (see _writeDataBlock)
##
sub writeENOSPC {
  my ($self, %args) = assertMinArgs(3, @_);
  $self->_writeDataBlocks(%args);
  if ($self->{_status} == 3) {
    my $machine = $self->{device}->getMachine();
    # RHEL7 machines don't distinguish EIO and ENOSPC yet.
    if (isMaipo($machine->getName())) {
      assertRegexpMatches(qr(: Input/output error), $machine->getStderr());
    } else {
      assertRegexpMatches(qr(: No space left on device),
                          $machine->getStderr());
    }
  } else {
    $self->_assertSuccess();
  }
}

#############################################################################
# Write the slice, then verify the data written.
#
# @oparam args  Passed onward to write
##
sub writeAndVerify {
  my ($self, @args) = assertMinArgs(1, @_);
  $self->write(@args);
  $self->verify(@args);
}

#############################################################################
# Assert that the genDataBlocks command succeeded.
##
sub _assertSuccess {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->{device}->getMachine();
  my $stdout = $machine->getStdout();
  my $stderr = $machine->getStderr();
  assertEqualNumeric(0, $self->{_status},
                     "genDataBlocks failed with status $self->{_status}"
                     . "\nstdout: $stdout\nstderr: $stderr");
}

#############################################################################
# Run genDataBlocks.
#
# @param action   The genDataBlocks action (such as "writeFiles")
# @param options  Command arguments specific to the action
#
# @return the succesful I/O statistics
##
sub _genDataBlocks {
  my ($self, $action, $options) = assertNumArgs(3, @_);
  my $machine = $self->{device}->getMachine();

  # Run the command.  Save the exit status for use by the caller.
  my $cmd = join(" ", "sudo", "genDataBlocks", @$options, $action);
  $log->info($machine->getName() . ": $cmd");
  $self->{_status} = $machine->sendCommand($cmd);

  # Return the I/O results, as a hash
  return { map { split(":", $_) } split("\n", $machine->getStdout()) };
}

#############################################################################
# Write a single stream of data blocks with optional dedupe and compression
# directly to a device or filesystem. Arguments are passed in name-value pairs.
#
# @param tag        The tag of the data
# @oparam compress  How much to compress, from 0 (none) to 0.96 (incredible)
# @oparam dedupe    How much to dedupe, from 0 (none) to 1 (total dedupe)
# @oparam direct    Use O_DIRECT on the device
# @oparam fsync     Use fsync before closing the device
# @oparam sync      Use O_SYNC on the device
##
sub _writeDataBlocks {
  my ($self, %args) = assertMinArgs(3, @_);
  assertDefined($args{tag});
  my $compress = $args{compress} || 0;
  my $dedupe   = $args{dedupe} || 0;
  my @options = ("--blockSize=$self->{blockSize}",
                 "--blockCount=$self->{blockCount}",
                 "--data=$args{tag},$dedupe,$compress");
  if ($args{direct}) {
    push(@options, "--direct");
  }
  if ($args{sync}) {
    push(@options, "--sync");
  }
  if ($args{fsync}) {
    push(@options, "--fsync");
  }

  my $action;
  if ($self->{fs}) {
    my $dir = makeFullPath($self->{fs}->getMountDir(), $args{tag});
    push(@options, "--fileCount=$self->{fileCount}", "--dir=$dir");
    $action = "writeFiles";
  } else {
    push(@options, "--device=" . $self->{device}->getSymbolicPath());
    if ($self->{offset} != 0) {
      push(@options, "--offset=$self->{offset}");
    }
    $action = "writeSlice";
  }
  my $results = $self->_genDataBlocks($action, \@options);

  # If any block was written, record the data set
  if (exists($results->{$args{tag}})) {
    $log->info("Wrote $results->{$args{tag}} blocks of $args{tag}");
    $self->{data}{$args{tag}}{compress}   = $compress;
    $self->{data}{$args{tag}}{dedupe}     = $dedupe;
  }
}

1;
