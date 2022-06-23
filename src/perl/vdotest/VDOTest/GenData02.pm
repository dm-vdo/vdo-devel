##
# Use genDataSet to generate and verify compressible data in multiple streams
#
# $Id$
##
package VDOTest::GenData02;

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
     dataSize          => $GB,
     # @ple Use a VDO device
     deviceType        => "lvmvdo",
     # @ple Enable compression for this test
     enableCompression => 1,
     # @ple Number of files to write per task
     fileCount         => 1024,
     # @ple This test requires a filesystem
     useFilesystem    => 1,
    );
##

########################################################################
##
sub testCompressibleData {
  my ($self)   = assertNumArgs(1, @_);
  my $fs       = $self->getFileSystem();
  my $numBytes = parseBytes($self->{dataSize});
  my $numFiles = $self->{fileCount};

  foreach my $compress (0, 0.3, 0.55, 0.85) {
    my $tag = "C" . int(100 * $compress);
    my $slice = $self->createSlice(fs         => $fs,
                                   fileCount  => $numFiles,
                                   totalBytes => $numBytes);
   my $task = Permabit::VDOTask::SliceOperation->new($slice, "writeAndVerify",
                                                     compress => $compress,
                                                     dedupe   => 0.25,
                                                     tag      => $tag);
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
