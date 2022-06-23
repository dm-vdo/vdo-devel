##
# This is an AsyncTask that will call ddWrite on a device, with supplied
# parameters.
#
# $Id$
##
package Permabit::VDOTask::DDWriteTask;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(Permabit::VDOTask);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @inherit
##
sub new {
  my ($invocant, $device, $ddParams) = assertNumArgs(3, @_);
  my $self = $invocant->SUPER::new();
  $self->{device} = $device;
  $self->{params} = $ddParams;
  $self->useDevice($device);
  return $self;
}

#############################################################################
# @inherit
##
sub taskCode {
  my ($self) = assertNumArgs(1, @_);
  $log->info("Starting ddwrite task");
  $self->{device}->ddWrite(%{$self->{params}});
  $log->info("ddwrite finished");
}

1;
