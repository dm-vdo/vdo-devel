##
# Run data writes versus discard
#
# $Id$
##
package VDOTest::Dual01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::Utils qw(sizeToText timeToText);
use Permabit::VDOTask::SliceOperation;
use Time::HiRes qw(time);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Run the test on real hardware (not virtual)
     clientClass => "ALBIREO-PMI",
     # @ple Use a VDO device
     deviceType  => "lvmvdo",
     # @ple the logical size of the VDO device
     logicalSize => 64 * $GB,
    );
##

########################################################################
##
sub testDual {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $volumeGroup = $self->createVolumeGroup($device, "onvdo");

  my $dualSize = $volumeGroup->getFreeBytes() / 2;
  my $discardDevice = $self->createTestDevice("linear",
                                              deviceName  => "discard",
                                              lvmSize     => $dualSize,
                                              volumeGroup => $volumeGroup);
  my $blockSize = $self->{blockSize};
  my $discardSlice = $self->createSlice(device     => $discardDevice,
                                        blockCount => $dualSize / $blockSize,
                                        blockSize  => $blockSize);
  my $discardTask = Permabit::VDOTask::SliceOperation->new($discardSlice,
                                                           "trim");

  my $genDevice = $self->createTestDevice("linear",
                                          deviceName  => "generate",
                                          volumeGroup => $volumeGroup);
  my $genFs = $self->createFileSystem($genDevice);
  my $genNumBytes = $genDevice->getSize() / 4;
  my $slice = $self->createSlice(fs         => $genFs,
                                 fileCount  => 1024,
                                 totalBytes => $genNumBytes);
  my $genTask = Permabit::VDOTask::SliceOperation->new($slice, "writeAndVerify",
                                                       dedupe => 0.25,
                                                       tag    => "gen");

  $self->getAsyncTasks()->addTask($discardTask);
  $self->getAsyncTasks()->addTask($genTask);

  $self->getAsyncTasks()->finish();
}

1;
