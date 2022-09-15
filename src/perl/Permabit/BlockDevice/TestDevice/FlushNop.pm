##
# Perl object that represents a test block device.
# $Id$
##
package Permabit::BlockDevice::TestDevice::FlushNop;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(Permabit::BlockDevice::TestDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# Get the current state of the flush freeze
##
sub isFlushFrozen {
  my ($self) = assertNumArgs(1, @_);
  my $data = $self->getMachine()->cat($self->makeSysfsPath("frozen"));
  chomp($data);
  return $data eq "true";
}

1;
