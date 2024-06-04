##
# Perl object that represents a block device that can corrupt
# data being written to or read from it based on config
# parameters we can set on the device. It also will generate
# output related to what data has been, but this functionality
# will only work if blktrace is run on the device.
#
# $Id$
##
package Permabit::BlockDevice::TestDevice::Managed::Corruptor;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertMinMaxArgs
  assertNumArgs
  assertType
);
use Permabit::Constants;
use Permabit::KernelModule;
use Permabit::ProcessCorruptor;
use Permabit::Utils qw(makeFullPath);

use base qw(Permabit::BlockDevice::TestDevice::Managed);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple the kernel module name
     moduleName => "pbitcorruptor",
    );
##

########################################################################
# Turn on current corruption type for reads.
##
sub enableCurrentRead {
  my ($self) = assertNumArgs(1, @_);
  $self->sendMessage("enable read");
}

########################################################################
# Turn on current corruption type for writes.
##
sub enableCurrentWrite {
  my ($self) = assertNumArgs(1, @_);
  $self->sendMessage("enable write");
}

########################################################################
# Turn on modulo corruption type for reads.
#
# @param frequency corrupt data if sector number modulo frequency == 0
##
sub enableModuloRead {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->setModuloRead($frequency);
  $self->enableCurrentRead();
}

########################################################################
# Turn on modulo corruption type for writes.
#
# @param frequency corrupt data if sector number modulo frequency == 0
##
sub enableModuloWrite {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->setModuloWrite($frequency);
  $self->enableCurrentWrite();
}

########################################################################
# Turn on random corruption type for reads.
#
# @param frequency corrupt data if random number modulo frequency == 0
##
sub enableRandomRead {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->setRandomRead($frequency);
  $self->enableCurrentRead();
}

########################################################################
# Turn on random corruption type for writes.
#
# @param frequency corrupt data if random number modulo frequency == 0
##
sub enableRandomWrite {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->setRandomWrite($frequency);
  $self->enableCurrentWrite();
}

########################################################################
# Turn on sequential corruption type for reads.
#
# @param frequency corrupt data if we have read frequency sectors
##
sub enableSequentialRead {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->setSequentialRead($frequency);
  $self->enableCurrentRead();
}

########################################################################
# Turn on sequential corruption type for writes.
#
# @param frequency corrupt data if we have written frequency sectors
##
sub enableSequentialWrite {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->setSequentialWrite($frequency);
  $self->enableCurrentWrite();
}

########################################################################
# Turn off current corruption type for reads.
##
sub disableCurrentRead {
  my ($self) = assertNumArgs(1, @_);
  $self->sendMessage("disable read");
}

########################################################################
# Turn off current corruption type for writes.
##
sub disableCurrentWrite {
  my ($self) = assertNumArgs(1, @_);
  $self->sendMessage("disable write");
}

########################################################################
# Parses, via ProcessCorruptor, the blkparse output for the device and returns
# the result.
#
# @oparam   blkparseFile  the file containing the blkparse output; defaults to
#                         last generated blkparse file
#
# @return the command result
##
sub parseBlockParse {
  my ($self, $blkparseFile) = assertMinMaxArgs(1, 2, @_);
  $blkparseFile //= $self->_getLastBlockParseFile();
  assertDefined($blkparseFile, "block parse file specified");

  my $args = {
                fileSpec      => $blkparseFile,
                host          => $self->getMachine()->getName(),
                allowFailure  => 1,
                machine       => $self->getMachine(),
             };
  return Permabit::ProcessCorruptor->new($self, $args)->run();
}

########################################################################
# Set modulo corruption type read parameters without changing the enable
# state.
#
# @param frequency corrupt data if sector number modulo frequency == 0
##
sub setModuloRead {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->sendMessage("parameters read modulo $frequency");
}

########################################################################
# Set modulo corruption type write parameters without changing the enable
# state.
#
# @param frequency corrupt data if sector number modulo frequency == 0
##
sub setModuloWrite {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->sendMessage("parameters write modulo $frequency");
}

########################################################################
# Set random corruption type read parameters without changing the enable
# state.
#
# @param frequency corrupt data if random number modulo frequency == 0
##
sub setRandomRead {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->sendMessage("parameters read random $frequency");
}

########################################################################
# Set random corruption type write parameters without changing the enable
# state.
#
# @param frequency corrupt data if random number modulo frequency == 0
##
sub setRandomWrite {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->sendMessage("parameters write random $frequency");
}

########################################################################
# Set sequential corruption type read parameters without changing the enable
# state.
#
# @param frequency corrupt data if we have read frequency sectors
##
sub setSequentialRead {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->sendMessage("parameters read sequential $frequency");
}

########################################################################
# Set sequential corruption type write parameters without changing the enable
# state.
#
# @param frequency corrupt data if we have written frequency sectors
##
sub setSequentialWrite {
  my ($self, $frequency) = assertNumArgs(2, @_);
  $self->sendMessage("parameters write sequential $frequency");
}

1;
