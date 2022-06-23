######################################################################
# Execute blkparse.
#
# $Id$
##
package Permabit::BlkParse;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(assertDefined assertNumArgs);

use base qw(Permabit::GenericCommand Exporter);

our @EXPORT_OK = qw(
  $BLKPARSE_SUFFIX
);
our $BLKPARSE_SUFFIX = "blkparse";

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %PROPERTIES =
  (
   # @ple base file name of trace file
   baseFileName   => undef,
   # the binary to use
   binary         => "blkparse",
   # Executable name
   name           => "blkparse",
  );

#############################################################################
# @inherit
##
sub getProperties {
  my $self = shift;
  return ( $self->SUPER::getProperties(),
           %PROPERTIES );
}

######################################################################
# @inherit
##
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  return join(" ",
              "-i", $self->{baseFileName},
              "-o", $self->getOutputFileName());
}

######################################################################
# @inherit
##
sub new {
  my ($invocant, $parent, $params) = assertNumArgs(3, @_);
  my $class = ref($invocant) || $invocant;
  my $self = $class->SUPER::new($parent, $params);
  assertDefined($self->{baseFileName});
  return $self;
}

######################################################################
# Returns the blkparse output file name.
#
# @return the blkparse output file name
##
sub getOutputFileName {
  my ($self) = assertNumArgs(1, @_);
  return join(".", $self->{baseFileName}, $BLKPARSE_SUFFIX);
}

1;
