##
# Stress test VDO behavior when VDO runs out of physical space, using
# direct I/O.
#
# We set the size of a slice (or partition) so that 1 slice can be stored
# in the physical space available on the VDO device.  We then divide the
# entire VDO device into as many slices that will fit in the logical space
# available.  Today (2015-Apr-27) I am seeing 16 slices.  We then generate
# as many data streams as there are slices.  Each slice gets its own
# primary data stream and one other data stream as an alternate data
# stream.  We then run the test loop some number of times.
#
# Each time through the loop we start one thread per slice.  Each thread
# writes a data stream onto its slice.  3/4 of the time it chooses its
# primary data stream, and 1/4 of the time it chooses its alternate data
# stream.  The writing will terminate when the device runs out of space.
# We then read the data written and verify that it matches our
# expectations.  Then 1/2 of the threads will trim its entire slice.
#
# Four of the data streams use fixed choices: (1) No dedupe and no
# compression; (2) Much dedupe and no compression; (3) No dedupe and much
# compression; (4) Much dedupe and much compression.  The other data
# streams use randomly chosen parameters.
#
# $Id$
##
package VDOTest::Full03;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(assertLTNumeric assertNumArgs);
use Permabit::Constants;
use Permabit::Utils qw(getRandomElement timeToText);
use Permabit::VDOTask::SliceOperation;
use Time::HiRes qw(time);

use base qw(VDOTest::FullBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple what class of machine to run the test on
     clientClass  => "ALBIREO-PMI",
     # @ple XXX relax the request latency limit for VDO-3180.  AsyncFull03
     #          exceeds this limit regularly, and we need a story to fix it.
     latencyLimit => 2 * $MINUTE,
    );
##

########################################################################
##
sub testOutOfSpace {
  my ($self) = assertNumArgs(1, @_);

  my $stats = $self->getDevice()->getVDOStats();
  my $blockCount = int($self->getUsableDataBlocks($stats) / 1000) * 1000;
  my $sliceCount = int($stats->{"logical blocks"} / $blockCount);
  $log->info("Block count: $blockCount");
  $log->info("Slice count: $sliceCount");
  $self->{_blockCount} = $blockCount;
  $self->{_blockSize}  = $stats->{"block size"};
  assertLTNumeric(4, $sliceCount);

  # Create the sections.  Each section gets it own slice and a primary data
  # description for its slice.
  my @compress = (0, 0.75, 0, 0.75, map { _randH() } (5 .. $sliceCount));
  my @dedupe = (0, 0, 0.75, 0.75, map { _randH() } (5 .. $sliceCount));
  my @sections;
  for my $number (1 .. $sliceCount) {
    my $offset = ($number - 1) * $self->{_blockCount};
    my $slice = $self->createSlice(
                                   blockCount => $self->{_blockCount},
                                   blockSize  => $self->{_blockSize},
                                   offset     => $offset,
                                  );
    push(@sections, {
                     number  => $number,
                     slice   => $slice,
                     primary => {
                                 compress => shift(@compress),
                                 dedupe   => shift(@dedupe),
                                 tag      => "data$number",
                                 direct   => 1,
                                },
                    });
  }

  # Assign a secondary data description to each section, using a primary data
  # description from a random section.
  for my $s (@sections) {
    $s->{secondary} = getRandomElement(\@sections)->{primary};
  }

  # In a loop, write, verify and optionally trim each section.
  for my $iteration (1 .. 10) {
    $log->info("Iteration $iteration write");
    my @writeTasks;
    for my $s (@sections) {
      # Use secondary data for 25% of the sections
      my $data = int(rand(4)) == 0 ? $s->{secondary} : $s->{primary};
      push(@writeTasks, Permabit::VDOTask::SliceOperation->new($s->{slice},
                                                               "writeENOSPC",
                                                               %$data));
    }
    $log->info("Writing took " . $self->_runTasks(\@writeTasks));
    $log->info("Iteration $iteration verify and trim");
    my @verifyTasks;
    for my $s (@sections) {
      # Trim 50% of the sections
      my $verifyType = int(rand(2)) ? "verifyAndTrim" : "verify";
      push(@verifyTasks,
           Permabit::VDOTask::SliceOperation->new($s->{slice}, $verifyType));
    }
    $log->info("Verification took " . $self->_runTasks(\@verifyTasks));
    $self->getDevice()->getMachine()->dropCaches();
  }
}

########################################################################
# Run a list of tasks
#
# @param  tasks  The tasks to run
#
# @return the time that it took to run (as a text string).
##
sub _runTasks {
  my ($self, $tasks) = assertNumArgs(2, @_);
  $self->getAsyncTasks()->addTasks(@$tasks);
  my $startTime = time();
  map { $_->start() } @$tasks;
  map { $_->result() } @$tasks;
  my $endTime = time();
  return timeToText($endTime - $startTime);
}

########################################################################
# Compute a random number with a harmonic distribution.  This will give us
# numbers that make 3:1 dedupe just as likely as 10:1 dedupe.  And similarly
# for compression.
#
# @return a random number with a harmonic distribution, suitable for the
#         compression fraction or dedupe fraction.
##
sub _randH {
  assertNumArgs(0, @_);
  return 1 - 1 / (1 + rand(19));
}

1;
