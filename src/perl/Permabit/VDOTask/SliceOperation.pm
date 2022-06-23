##
# This is an AsyncTask that will do an operation on a Permabit::GenSlice.  When
# the operation is complete, the Permabit::GenSlice object will be updated to
# reflect the results of the operation.
#
# $Id$
##
package Permabit::VDOTask::SliceOperation;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Assertions qw(assertMinArgs assertNumArgs assertType);

use base qw(Permabit::VDOTask);

###############################################################################
# Set up a new Permabit::VDOTask::SliceOperation.
#
# @param slice      The Permabit::GenSlice.
# @param operation  The name of the Permabit::GenSlice method to call.
# @oparam args      The arguments to the Permabit::GenSlice method.
#
# return the new Permabit::VDOTask::SliceOperation.
##
sub new {
  my ($invocant, $slice, $operation, %args) = assertMinArgs(3, @_);
  my $self = $invocant->SUPER::new();
  $self->{args}      = \%args;
  $self->{imported}  = 0;
  $self->{operation} = $operation;
  $self->{slice}     = $slice;
  assertType("Permabit::GenSlice", $slice);
  $self->useDevice($slice->getDevice());
  if ($slice->getFileSystem()) {
    $self->useFileSystem($slice->getFileSystem());
  }
  return $self;
}

###############################################################################
# @inherit
##
sub taskCode {
  my ($self) = assertNumArgs(1, @_);
  my $operation = $self->{operation};
  $self->{slice}->$operation(%{$self->{args}});
  return $self->{slice}->exportData();
}

###############################################################################
# @inherit
##
sub result {
  my ($self) = assertNumArgs(1, @_);
  my $result = $self->SUPER::result();
  if (!$self->{imported}) {
    $self->{slice}->importData($result);
    $self->{imported} = 1;
  }
  return $result;
}

1;
