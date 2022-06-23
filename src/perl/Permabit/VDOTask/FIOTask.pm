##
# This is an AsyncTask that will call fio on a device, with supplied
# parameters.
#
# $Id$
##
package Permabit::VDOTask::FIOTask;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::CommandString::FIO;

use base qw(Permabit::VDOTask);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @inherit
##
sub new {
  my ($invocant, $test, $device, $params) = assertNumArgs(4, @_);
  my $self = $invocant->SUPER::new();
  $self->{test}   = $test;
  $self->{device} = $device;
  $self->{params} = $params;
  $self->useDevice($device);
  return $self;
}

#############################################################################
# @inherit
##
sub taskCode {
  my ($self) = assertNumArgs(1, @_);
  $log->info("Starting fio task");
  my %params = (
                filename => $self->{device}->getSymbolicPath(),
                %{$self->{params}}
                );
  my $fio = Permabit::CommandString::FIO->new($self->{test}, \%params);
  $self->{device}->getMachine()->assertExecuteCommand($fio);
  $log->info("fio finished");
}

1;
