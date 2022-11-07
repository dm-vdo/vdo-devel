##
# Use genDataSet to generate, verify and delete data in multiple streams.
#
# This test writes and deletes more data than will fit in a standard test VDO
# device.  This is meant to test that we are properly reclaiming deleted space
# in a timely manner.
#
# $Id$
##
package VDOTest::GenData03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertLTNumeric assertNumArgs);
use Permabit::Constants;
use Permabit::LabUtils qw(isVirtualMachine);
use Permabit::Utils qw(parseBytes);
use Permabit::VDOTask::GenData03Task;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Amount of data to write in each dataset
     dataSize       => 100 * $MB,
     # @ple Use a VDO device
     deviceType     => "lvmvdo",
     # @ple Number of tasks to launch
     numTasks       => 4,
     # @ple This test requires a filesystem
     useFilesystem  => 1,
    );
##

########################################################################
##
sub testGenData {
  my ($self) = assertNumArgs(1, @_);
  $self->{dataSize} = parseBytes($self->{dataSize});

  foreach my $taskNumber (1 .. $self->{numTasks}) {
    my $task = Permabit::VDOTask::GenData03Task->new($taskNumber,
                                                     $self->{numTasks},
                                                     $self->getFileSystem(),
                                                     $self->{dataSize},
                                                     $self->{blockSize});
    $self->getAsyncTasks()->addTask($task);
  }

  my $beforeStats = $self->getVDOStats();
  if (defined($beforeStats)) {
    $beforeStats->logStats("before");
  }
  $self->getAsyncTasks()->finish();

  # Ensure all filesystem activity has finished before querying stats.
  $self->getFileSystem()->unmount();

  my $afterStats = $self->getVDOStats();
  if (defined($beforeStats) && defined($afterStats)) {
    $afterStats->logStats("after");

    # There should be no albireo timeouts during the test (unless it is a
    # virtual machine or a low memory test).
    if (!isVirtualMachine($self->getDevice()->getMachine()->getName())
        && !$self->{lowMemoryTest}) {
      assertEqualNumeric($beforeStats->{"dedupe advice timeouts"},
                         $afterStats->{"dedupe advice timeouts"});
    }

    # If we are handling flushes, all the file systems we test with are
    # journaled and should have done some REQ_FLUSH accesses and some
    # REQ_FUA accesses.
    assertLTNumeric($beforeStats->{"bios in flush"},
                    $afterStats->{"bios in flush"});
    assertLTNumeric($beforeStats->{"bios in fua"},
                    $afterStats->{"bios in fua"});

    # Make sure we're not miscounting flushes [VDO-3535].
    assertEqualNumeric(0, $afterStats->{"bios in progress flush"});
    assertEqualNumeric(0, $afterStats->{"bios in progress fua"});
    assertEqualNumeric(0, $afterStats->{"bios in progress write"});

    # If the filesystem issues discards, we should have seen some.  For
    # example, ext4 does discards, and ext3 doesn't. (There may also be a
    # cronjob calling fstrim during the test, so we can't assert that there
    # are no discards when the filesystem isn't using discards.)
    if ($self->getFileSystem()->usingDiscard()) {
      assertLTNumeric($beforeStats->{"bios in discard"},
                      $afterStats->{"bios in discard"});
    }
  }
}

1;
