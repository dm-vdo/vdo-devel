##
# Base class for kvdo perf tests
#
# $Id$
##
package VDOTest::VDOPerfBase;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple fill the index before testing
  albFill               => 1,
  # @ple use hardware raid by default
  clientClass           => "VDO-PMI",
  # @ple The type of VDO device to use
  deviceType            => "lvmvdo",
  # @ple Our SSD arrays can only index 1TB so no point for a large index
  memorySize            => 0.25,
  # @ple Ask rsvpd to randomize its list of available hosts before
  # selecting; this will help with evening SSD wearout.
  randomizeReservations => 1,
  # @ple Use the largest possible slabs for performance tests
  slabBits              => $SLAB_BITS_LARGE,
  # @ple preload the page cache at startup
  vdoWarmup             => 1,
);
##

#############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);

  $self->SUPER::set_up();

  my $machine = $self->getUserMachine();
  $machine->waitForDiskSelfTests();

  # Make sure all CPUs are running full speed.
  #
  # Availability depends on the CPU.
  #
  # The scaling_governor approach appears to be available only for MGH
  # machines at the moment.
  my $testGovernorPath =
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor";
  my $governorPaths = "/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor";
  if ($machine->pathExists($testGovernorPath)) {
    my $cmd = "echo performance | sudo tee $governorPaths";
    $machine->runSystemCmd($cmd);
  }
}

1;
