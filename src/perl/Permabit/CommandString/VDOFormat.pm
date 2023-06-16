###############################################################################
# A command for running the vdoFormat command
#
# $Id$
##
package Permabit::CommandString::VDOFormat;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs assertTrue);
use Permabit::Constants;

use base qw(Permabit::CommandString);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %COMMANDSTRING_PROPERTIES
  = (
     # just print help
     help               => undef,
     # force formatting despite existing VDO
     force              => undef,
     # executable name
     name               => "vdoformat",
     # the absolute file name of the underlying file or device
     storage            => undef,
     # verbosity flag
     verbose            => 0,
    );

our %COMMANDSTRING_INHERITED_PROPERTIES
  = (
     # VDO logical size in lvm syntax (default unit is MB)
     logicalSize  => undef,
     # VDO slab bit count
     slabBits     => undef,
    );

###############################################################################
# @inherit
##
sub new {
  my $invocant = shift;
  my $self = $invocant->SUPER::new(@_);
  $self->assertTrue(defined($self->{storage}) || defined($self->{help}));
  return $self;
}

###############################################################################
# @inherit
##
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  my @args;
  if ($self->{help}) {
    $self->addSimpleOption(\@args, "help",          "--help");
  } else {
    $self->addSimpleOption(\@args, "force",         "--force");
    $self->addValueOption(\@args,  "logicalSize",   "--logical-size");
    $self->addValueOption(\@args,  "slabBits",      "--slab-bits");
    $self->addValueOption(\@args,  "albireoMem",    "--uds-memory-size");
    $self->addSimpleOption(\@args, "albireoSparse", "--uds-sparse");
    $self->addSimpleOption(\@args, "verbose",       "--verbose");
    push (@args, $self->{storage});
  }
  return @args;
}

1;
