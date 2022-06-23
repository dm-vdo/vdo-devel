######################################################################
# Execute processTrace.py
#
# $Id$
##
package Permabit::ProcessTracer;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(assertDefined assertNumArgs);
use Permabit::Utils qw(makeFullPath);
use Permabit::SystemUtils qw(pythonCommand);

use base qw(Permabit::GenericCommand);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %PROPERTIES =
  (
   # @ple the binary to use
   binary             => "src/python/devices/tracer/processTrace.py",
   # @ple file in which to write debug output; undef => no debug output
   debugFile          => undef,
   # @ple boolean indicating if should display mismatches
   displayMismatches  => 1,
   # @ple specifies files to process
   fileSpec           => undef,
   # @ple Executable name
   name               => "processTrace.py",
   # @ple output file name; undef => stdout
   outputFile         => undef,
   # @ple boolean indicating if sectors seen only once, rather than mismatches,
   #      should be output
   singles            => 0,
   # @ple Python module path
   pythonPath       => "src/python",
  );

my %INHERITED_PROPERTIES =
  (
   # @ple machine on which command will be run
   machine            => undef,
  );

######################################################################
# @inherit
##
sub buildMainCommand {
  my ($self) = assertNumArgs(1, @_);

  my $progPath
    = $self->{machine}->makeNfsSharePath($self->SUPER::buildMainCommand());
  $self->{env}->{"PYTHONPATH"} =
    $self->{machine}->makeNfsSharePath($self->{pythonPath});
  my $env = $self->SUPER::buildEnvironment();

  my $options = "";
  if (defined($self->{debugFile})) {
    $options = join(" ", $options, "--debug", $self->{debugFile});
  }
  if (defined($self->{outputFile})) {
    $options = join(" ", $options, "--outfile", $self->{outputFile});
  }
  if (!$self->{displayMismatches}) {
    $options = join(" ", "--quiet");
  }
  if ($self->{singles}) {
    $options = join(" ", "--singles");
  }

  return pythonCommand("$env $progPath", "$options $self->{fileSpec}", 1);
}

#############################################################################
# @inherit
##
sub getInheritedProperties {
  my $self = shift;
  return ( $self->SUPER::getInheritedProperties(),
           %INHERITED_PROPERTIES );
}

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
sub new {
  my ($invocant, $parent, $params) = assertNumArgs(3, @_);
  my $class = ref($invocant) || $invocant;
  my $self = $class->SUPER::new($parent, $params);
  assertDefined($self->{fileSpec});
  assertDefined($self->{host});
  return $self;
}

1;
