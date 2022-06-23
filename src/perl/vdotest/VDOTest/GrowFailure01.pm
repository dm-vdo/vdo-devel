##
# Test that failing to grow the logical size of a VDO doesn't fail horribly.
#
# $Id$
##
package VDOTest::GrowFailure01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEvalErrorMatches
  assertNumArgs
  assertRegexpMatches
);
use Permabit::Constants;
use Permabit::LabUtils qw(getTotalRAM);

use base qw(VDOTest);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple what type of VDO device to set up
     deviceType  => "lvmvdo",
     # @ple the starting logical size
     logicalSize => "50T",
    );
##

#############################################################################
##
sub testOutOfMemory {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  # Grow by 50T increments, which requires about 70M additional memory for the
  # block map tree.
  my $currentCursor;
  while (1) {
    $currentCursor = $machine->getKernelJournalCursor();
    eval {
      $device->growLogical($self->{logicalSize} + 50 * $TB);
    };
    if ($EVAL_ERROR) {
      last;
    }
    $self->{logicalSize} += 50 * $TB;
  }

  assertEvalErrorMatches(qr/\s*Cannot prepare to grow logical /);
  assertRegexpMatches(qr/ Could not allocate \d+ bytes for /,
                      $machine->getKernelJournalSince($currentCursor));
}

1;
