##
# Base class for tests utilizing Corruptor; this includes tests of Tracer
# which use Corruptor to generate corruption for Tracer to identify.
#
# $Id$
##
package VDOTest::CorruptionBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertDefined
                            assertMinMaxArgs
                            assertNumArgs
                            assertTrue);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple Setup a corruptor device
  deviceType => "corruptor",
  # @ple don't suppress cleanup on verify failures
  suppressCleanupOnError  => [],
  # @ple arguments to include on every slice i/o
  _defaultSliceArgs => { direct => 1 },
);
##

#############################################################################
# Assert that the error specified is the expected error.
#
# @param  error         the error to check
##
sub __assertError {
  my ($self, $error) = assertNumArgs(2, @_);
  my $errorString = "block [0-9]+ [unrecognized|compare failure]";
  assertTrue($error =~ /$errorString/ || 0);
}

#############################################################################
# Perform slice verification using the slice default arguments and any others
# specified.
#
# @param  slice         the slice to verify
# @oparam args          additional args hash
##
sub _verifySlice {
  my ($self, $slice, $args) = assertMinMaxArgs([{}], 2, 3, @_);
  $slice->verify(%{$self->{_defaultSliceArgs}}, %{$args});
}

#############################################################################
# Perform slice verification using the slice default arguments and any others
# specified checking that the verification fails.
#
# @param  slice         the slice to verify
# @oparam args          additional args hash
##
sub _verifySliceFailure {
  my ($self, $slice, $args) = assertMinMaxArgs([{}], 2, 3, @_);
  eval {
    $self->_verifySlice($slice, %{$args});
  };
  $self->__assertError($EVAL_ERROR);
}

#############################################################################
# Perform slice verification using the slice default arguments and any others
# specified.  The verification is allowed to fail; if it does the failure is
# checked as being of the correct kind.
#
# @param  slice         the slice to verify
# @oparam args          additional args hash
##
sub _verifySlicePotentialFailure {
  my ($self, $slice, $args) = assertMinMaxArgs([{}], 2, 3, @_);
  eval {
    $self->_verifySlice($slice, %{$args});
  };
  if ($EVAL_ERROR) {
    $self->__assertError($EVAL_ERROR);
  }
}

#############################################################################
# Perform slice write using the slice default arguments and any others
# supplied.
#
# @param  slice     the slice to write
# @oparam args      additional args hash
##
sub _writeSlice {
  my ($self, $slice, $args) = assertMinMaxArgs([{}], 2, 3, @_);
  $slice->write(%{$self->{_defaultSliceArgs}}, %{$args});
}

1;
