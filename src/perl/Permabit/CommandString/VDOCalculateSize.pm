###############################################################################
# A command for running the vdoFormat command
#
# $Id$
##
package Permabit::CommandString::VDOCalculateSize;

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
     "help"                 => undef,
     "block-map-cache-size" => "32K",
     "name"                 => "vdoCalculateSize",
     "index-memory-size"    => "1",
     "logical-size"         => "1T",
     "physical-size"        => "300G",
     "slab-bits"            => undef,
     "slab-size"            => undef,
     "sparse-index"         => undef,
  );

###############################################################################
# @inherit
##
sub new {
  my $invocant = shift;
  my $self = $invocant->SUPER::new(@_);
  return $self;
}

###############################################################################
# @inherit
##
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  my @args;
  if ($self->{help}) {
    $self->addSimpleOption(\@args, "help",                "--help");
  } else {
    $self->addValueOption(\@args, "block-map-cache-size", "--block-map-cache-size");
    $self->addValueOption(\@args, "index-memory-size",    "--index-memory-size");
    $self->addValueOption(\@args, "logical-size",         "--logical-size");
    $self->addValueOption(\@args, "physical-size",        "--physical-size");
    if (defined($self->{"slab-bits"})) {
      $self->addValueOption(\@args, "slab-bits",          "--slab-bits");
    }
    if (defined($self->{"sparse-index"})) {
      $self->addSimpleOption(\@args, "sparse-index",      "--sparse-index");
    }
    if (defined($self->{"slab-size"})) {
      $self->addValueOption(\@args, "slab-size",          "--slab-size");
    }
  }
  return @args;
}

1;
