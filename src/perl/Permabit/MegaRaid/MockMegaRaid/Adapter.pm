#############################################################################
# Mock Permabit::MegaRaid::Adapter class, for testing
#
# We subclass Permabit::MegaRaid::Adapter and override _getResults to use
#  our MockMegaRaid::MegaCli object.
#
# $Id$
##
package Permabit::MegaRaid::MockMegaRaid::Adapter;

use strict;
use warnings FATAL => qw(all);

use Permabit::Assertions qw(
  assertDefined
  assertNumDefinedArgs
);

use base qw(Permabit::MegaRaid::Adapter);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @inherit
# Override the cli factory so we don't actually run commands.
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;

  my $mockCli = 'Permabit::MegaRaid::MockMegaRaid::MegaCli';
  return $class->SUPER::new(@_,
                            _cliFactory => $mockCli,
                            pciAddress  => "0000:02:00.0",
                           );
}

1;
