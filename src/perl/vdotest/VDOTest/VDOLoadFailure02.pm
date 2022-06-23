##
# Test that failing to load VDO doesn't die horribly.  The failure is due to
# wanting too much memory and therefore a memory allocation fails.
#
# $Id$
##
package VDOTest::VDOLoadFailure02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertEvalErrorMatches assertNumArgs assertTrue);
use Permabit::Constants;
use Permabit::LabUtils qw(getTotalRAM);

use base qw(VDOTest);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # what type of VDO device to set up
     deviceType   => "vdo",
     # physical size of the VDO
     physicalSize => "1GB",
     # the number of bits in the VDO slab
     slabBits     => $SLAB_BITS_TINY,
    );
##

#############################################################################
##
sub testOutOfMemory {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  $device->stop();
  # Set the block map cache size to RAM.
  $device->{blockMapCacheSize} = getTotalRAM($machine->getName());

  my $currentLogLine = 0;
  while (1) {
    $currentLogLine = $machine->getKernLogSize();
    eval {
      $device->start();
    };
    if ($EVAL_ERROR) {
      last;
    }
    $device->stop();
    $device->{blockMapCacheSize} += 20 * $MB;
  }

  assertEvalErrorMatches(qr/\s*Failed while running sudo dmsetup create /);
  assertTrue($machine->searchKernLog($currentLogLine + 1,
                                     " aborting load: System error 12"));
}

1;
