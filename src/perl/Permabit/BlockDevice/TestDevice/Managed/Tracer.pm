##
# Perl object that represents a block device that generates
# diagnostic output for other devices like vdo. It requires
# blktrace to be run on the device in order to generate
# that output.
#
# $Id$
##
package Permabit::BlockDevice::TestDevice::Managed::Tracer;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertDefined
                            assertFalse
                            assertMinMaxArgs
                            assertNumArgs);
use Permabit::BlkParse qw($BLKPARSE_SUFFIX);
use Permabit::Constants;
use Permabit::KernelModule;
use Permabit::ProcessTracer;
use Permabit::SystemUtils qw(assertCommand);
use Permabit::Utils qw(makeFullPath);

use base qw(Permabit::BlockDevice::TestDevice::Managed);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple boolean indicating if should assert on finding a mismatch
     assertOnMismatch   => 1,
     # @ple the kernel module name
     moduleName         => "pbittracer",
     # @ple boolean indicating if device should post-process block trace output
     #      after stopping block tracing
     _processBlockTrace => 1,
    );
##

# These are the properties inherited from the testcase.  Note that testcase
# base classes like VDOTest directly copy this hash into its own properties.
# The defaults here are then used.
our %BLOCKDEVICE_INHERITED_PROPERTIES
  = (
     # whether to trace on each sector (1) or each 4k block (8).
     traceSectors => 1,
    );

########################################################################
# @inherit
##
sub makeTableLine {
  my ($self) = assertNumArgs(1, @_);
  return join(' ',
              $self->SUPER::makeTableLine(),
              $self->{traceSectors});
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::activate();
  $self->addDeactivationStep(sub { $self->parseOnStop(); }, 0);
}

########################################################################
# @inherit
##
sub postActivate {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{tracing}) {
    $self->enable();
  }
  $self->SUPER::postActivate();
}

########################################################################
# @inherit
##
sub disable {
  my ($self) = assertNumArgs(1, @_);
  if ($self->isTracing()) {
    $self->{_processBlockTrace} = 1;
    $self->{_parsePath}
      = makeFullPath($self->{runDir},
                     join(".",
                          $self->{_blktrace}->getBaseFileName(),
                          $BLKPARSE_SUFFIX));
  } else {
    delete $self->{_parsePath};
  }

  $self->SUPER::disable();
}

########################################################################
# @inherit
##
sub parseOnStop {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->{_parsePath}) {
    return;
  }

  my $result = $self->parseBlockParse($self->{_parsePath});
  assertFalse(($self->{assertOnMismatch} && ($result->{status} != 0)),
              "no sector hash mismatch detected");
}

########################################################################
# @inherit
##
sub teardown {
  my ($self) = assertNumArgs(1, @_);
  my $trace = $self->{_blktrace};
  my $coreFileName;
  if (defined($trace)) {
    $coreFileName = $trace->getCoreFileName();
  }

  $self->SUPER::teardown();

  if (!$coreFileName) {
    return;
  }

  my $blkparsePath
    = makeFullPath($self->{runDir},
                   join(".", $coreFileName, "*", $BLKPARSE_SUFFIX));
  my $result = assertCommand($self->getMachineName(), "ls -1 $blkparsePath");
  my @files = split("\n", $result->{stdout});
  if (scalar(@files) <= 1) {
    return;
  }

  $result = $self->parseBlockParse($blkparsePath);
  assertFalse(($self->{assertOnMismatch} && ($result->{status} != 0)),
              "no sector hash mismatch detected");
}

########################################################################
# Parses, via ProcessTracer, the blkparse output for the device and returns
# the result.
#
# @oparam   blkparseFile  the file containing the blkparse output; defaults to
#                         last generated blkparse file
# @oparam   singles       if true, return the blocks that were only seen once
#
# @return the command result
##
sub parseBlockParse {
  my ($self, $blkparseFile, $singles) = assertMinMaxArgs([undef, 0], 1, 3, @_);
  $blkparseFile //= $self->_getLastBlockParseFile();
  assertDefined($blkparseFile, "block parse file specified");

  my $args = {
              fileSpec      => $blkparseFile,
              host          => $self->getMachineName(),
              allowFailure  => 1,
              singles       => $singles,
              machine       => $self->getMachine(),
             };
  return Permabit::ProcessTracer->new($self, $args)->run();
}

1;
