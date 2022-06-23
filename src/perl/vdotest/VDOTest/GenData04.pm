##
# Use genDataSet to generate and verify data in multiple streams
#
# $Id$
##
package VDOTest::GenData04;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants;
use Permabit::LabUtils qw(isVirtualMachine);
use Permabit::Utils qw(parseBytes);
use Permabit::VDOTask::SliceOperation;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Amount of data to write in each of the four datasets
     dataSize      => $GB,
     # @ple Use a VDO device
     deviceType    => "lvmvdo",
     # @ple This test requires a filesystem
     useFilesystem => 1,
    );
##

########################################################################
# Create a filesystem, generate data into it in parallel from
# different streams, and then verify it.
##
sub testParallelData {
  my ($self)   = assertNumArgs(1, @_);
  my $fs       = $self->getFileSystem();
  my $numBytes = parseBytes($self->{dataSize});

  foreach my $numFiles (1, 32, 1024, 32768) {
    my $slice = $self->createSlice(fs         => $fs,
                                   fileCount  => $numFiles,
                                   totalBytes => $numBytes);
    my $task = Permabit::VDOTask::SliceOperation->new($slice, "writeAndVerify",
                                                      dedupe => 0.25,
                                                      tag    => "N$numFiles");
    $self->getAsyncTasks()->addTask($task);
  }

  my $beforeStats = $self->getVDOStats();
  $self->getAsyncTasks()->finish();
  my $afterStats = $self->getVDOStats();

  if (defined($beforeStats)
      && defined($afterStats)
      && !isVirtualMachine($self->getDevice()->getMachine()->getName())
      && !$self->{lowMemoryTest}) {
    assertEqualNumeric($beforeStats->{"dedupe advice timeouts"},
                       $afterStats->{"dedupe advice timeouts"});
  }
}

1;
