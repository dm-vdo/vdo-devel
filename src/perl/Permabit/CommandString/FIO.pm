###############################################################################
# A command for running the fio command.
#
# $Id$
##
package Permabit::CommandString::FIO;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Carp qw(croak);
use Permabit::Assertions qw(assertNumArgs assertDefined);
use Permabit::Constants qw($KB);

use base qw(Permabit::CommandString);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

# Object fields
# NOTE: See `man fio` for details about what specific fio options do
our %COMMANDSTRING_PROPERTIES
  = (
     # albgenstream
     albGenStream           => undef,
     # blocksize in Bytes
     blockSize              => 4 * $KB,
     # if set, fio will fill the IO buffers with this pattern
     bufferPattern          => undef,
     # if a compress percent is set, then we will either use regular fio or
     # albGenStream to generate data, depending on dedupe percent
     compressPercent        => undef,
     # unlink test files after test and prevent filesystem overwrites (unlink
     # is 1)
     cleanupBenchmark       => 1,
     # CPU core IDs that are allowed for use in threads, used to lock cores
     cpusAllowed            => undef,
     # debug output (possible values are superabundant; 'all' is most useful)
     debug                  => undef,
     # direct IO bool (fio default is false)
     directIo               => 1,
     # the target directory to write benchmark files to
     directory              => undef,
     # fio needs sudo when writing to raw devices
     doSudo                 => 1,
     # whether to invoke fsync at the end of the test
     endFsync               => undef,
     # pre allocate the file or not (none, posix, or keep).
     fallocate              => undef,
     # the target filename to write benchmark files to
     filename               => undef,
     # how often (after how many I/O operations) to call fsync during the test
     fsync                  => undef,
     # Option used to make fio output easier to parse
     group_reporting        => undef,
     # Reduce gettimeofday calls/overhead
     gtod_reduce            => 1,
     # pending IO ct (fio default is 1)
     ioDepth                => 16,
     # minimum number of I/Os at once to ask for at once during the retrieval
     # process from the kernel. default is 1 which means the number of I/Os
     # retreived will be between 1 and ioDepth.
     ioDepthBatchComplete   => undef,
     # number of I/Os to submit at once. Default: iodepth
     ioDepthBatchSubmit     => undef,
     # io engine (fio default is "sync")
     ioEngine               => "libaio",
     # ioscheduler (fio default is <system conf>)
     ioScheduler            => undef,
     # the amount of data to write (if defaulting to the logical size
     # is not desired)
     ioSize                 => undef,
     # IO pattern (read, write, randread, randwrite, rw, or randrw)
     ioType                 => "randrw",
     # modifies IO pattern offset generation
     ioTypeModifier         => undef,
     # controls how the ioTypeModifier behaves
     ioTypeModifierSequence => undef,
     # iteration count (fio default is 1)
     loops                  => undef,
     # the job name param which fio requires
     jobName                => "generic_job_name",
     # number of jobs to run (threads or processes)
     jobs                   => 1,
     # fio job file where we can define multiple jobs at the same time.  If the
     # jobFile is set, it will override the other job parameters that are
     # passed in to this object.
     jobFile                => undef,
     # whether or not to retrieve latency information.  If turned on, will turn
     # off gtod_reduce. Off by default.
     latency                => 0,
     # ask fio to attempt to lock up extra memory (for values > 1/2 the
     # physical memory, this option only works when using threads).
     lockMem                => undef,
     # Option used to make fio output easier to parse
     minimal                => undef,
     # Executable loc
     name                   => "fio",
     # flag to not try to hit every block with random I/O.  Off by default.
     norandommap            => 0,
     # offset at which to start reading/writing
     offset                 => undef,
     # offset for each subjob
     offsetIncrement        => undef,
     # fio's log file where it puts its results, STDOUT if undef
     outFile                => undef,
     # Bandwidth limit in bytes/second; applies to both read and write rates
     # unless specified separately.  Uses same format as "rate" parameter for
     # FIO: 1000000, 1000k, and 1m are the same amount.  Separate read/write
     # can be specified like this: "1m,500k"" would limit read rate to 1
     # MB/second and write rate to 500 KB/second.
     rate                   => undef,
     # the maximum amount of time to run (or total time, if writePerJob is not
     # set)
     runTime                => undef,
     # percentage of a mixed workload that should be reads (Fio default: 50).
     rwmixread              => 70,
     # scramble buffers to cause dedupe devices to actually do work
     # (will be on by default unless a conflicting filling option is selected)
     scrambleBuffers        => undef,
     # use softrandommap to try to pick unique random addresses
     softrandommap          => undef,
     # use threads instead of procs
     thread                 => 1,
     # If true, use the random number generator from the OS; otherwise use
     # FIO's own generator.
     useOsRand              => undef,
     # set the verify mode (md5,meta,sha1,etc...) and run the verify at the
     # end.
     verify                 => undef,
     # how many blocks to write before verifying them. Default is to write all
     # the blocks and then do the verify (this can require a lot of memory).
     verifyBacklog          => undef,
     # Whether to dump out the bad block on verification failure
     verifyDump             => undef,
     # Whether to stop at the first verification failure
     verifyFatal            => undef,
     # write amount, per job, in Bytes
     writePerJob            => undef,
     # initialize write buffers with zeroes instead of random data
     zeroBuffers            => undef,
    );

###############################################################################
# @inherit
##
sub new {
  my $invocant = shift;
  my $self = $invocant->SUPER::new(@_);

  # verify required properties are set
  assertDefined($self->{directory} // $self->{filename},
                "a filename or directory must be specified");
  if (!defined($self->{jobFile})) {
    assertDefined($self->{blockSize});
    assertDefined($self->{ioType});
    assertDefined($self->{jobName});
    assertDefined($self->{jobs});
    if (!$self->{albGenStream}) {
      assertDefined($self->{writePerJob} // $self->{runTime},
                    "Either writePerJob or runTime must be defined");
    }
  }

  if (($self->{ioEngine} eq "libaio") && !$self->{directIo}) {
    # http://lse.sourceforge.net/io/aio.html
    $log->warn("libaio is synchronous when not using O_DIRECT");
  }
  if ($self->{ioDepth}
      && ($self->{ioDepth} > 1)
      && ($self->{ioEngine} =~ /sync/)) {
    $log->warn("ioDepth > 1 has no effect with a sync ioEngine");
  }
  # scrambleBuffers=1 is our default unless any filling option is specified
  # since it's enabled by default in fio itself.
  if (!defined($self->{bufferPattern})
      && !defined($self->{scrambleBuffers})
      && !defined($self->{zeroBuffers})) {
    $self->{scrambleBuffers} = 1;
  }

  # Check that only one filling option has been selected.
  if (defined($self->{zeroBuffers}) && $self->{zeroBuffers}
      && defined($self->{scrambleBuffers}) && $self->{scrambleBuffers}) {
    _requireExclusive(qw(scrambleBuffers zeroBuffers));
  }
  if (defined($self->{bufferPattern})) {
    if (defined($self->{scrambleBuffers} && $self->{scrambleBuffers})) {
      _requireExclusive(qw(bufferPattern scrambleBuffers));
    }
    if (defined($self->{zeroBuffers} && $self->{zeroBuffers})) {
      _requireExclusive(qw(bufferPattern zeroBuffers));
    }
  }

  if (defined($self->{albGenStream})) {
    my $warnMsg = "has no effect when using albGenStream";
    if ($self->{verify}) {
      # Some verify methods might actually be ok but "meta" is not.
      croak("verify not supported with albGenStream");
    }
    if ($self->{scrambleBuffers}) {
      $log->warn("scrambleBuffers $warnMsg");
    }
    if ($self->{writePerJob}) {
      $log->warn("write: $self->{writePerJob}: $warnMsg");
    }
    if ($self->{runtime}) {
      $log->warn("runtime: $self->{runtime}: $warnMsg");
    }
    if ($self->{ioType} =~ /rw$/) {
      $log->warn("mixed r/w workloads have unpredictible dedupe");
    }
  }

  # If we're specifying that fio should run its worker threads only on
  # certain cores, let's start the process on them right off the bat.
  if (!defined($self->{cpuAffinity}) && !defined($self->{cpuAffinityList})) {
    $self->{cpuAffinityList} = $self->{cpusAllowed};
  }

  return $self;
}

###############################################################################
# Fail because two exclusive options have both been selected.
#
# @param name1  The first option name
# @param name2  The second option name
##
sub _requireExclusive {
  my ($name1, $name2) = assertNumArgs(2, @_);
  croak("Only one of --$name1 and --$name2 may be used");
}

###############################################################################
# @inherit
##
sub getArguments {
  my ($self) = assertNumArgs(1, @_);
  my @args;

  # Set fio command parameters
  $self->addSimpleOption(\@args, "minimal", "--minimal");
  $self->addValueOption(\@args,  "outFile", "--output");

  # Set job parameters
  if (defined($self->{jobFile})) {
    push(@args, $self->{jobFile});
  } else {
    $self->{_ioType} = $self->{ioType};
    if (defined($self->{ioTypeModifier})) {
      $self->{_ioType} .= ":" . "$self->{ioTypeModifier}";
    }
    # XXX Ordering of args does matter to FIO, also with this scheme, it's not
    #     going to be able to express more complex jobs.
    # set required args
    $self->addValueOption(\@args,  "blockSize",        "--bs");
    $self->addValueOption(\@args,  "ioTypeModifierSequence", "--rw_sequencer");
    $self->addValueOption(\@args,  "_ioType",          "--rw");
    $self->addValueOption(\@args,  "jobName",          "--name");
    $self->addValueOption(\@args,  "directory",        "--directory");
    $self->addValueOption(\@args,  "filename",         "--filename");
    $self->addValueOption(\@args,  "jobs",             "--numjobs");
    $self->addValueOption(\@args,  "writePerJob",      "--size");

    # set optional args
    $self->addSimpleOption(\@args, "thread",           "--thread");
    $self->addSimpleOption(\@args, "norandommap",      "--norandommap");
    $self->addSimpleOption(\@args, "zeroBuffers",      "--zero_buffers");
    $self->addValueOption(\@args,  "randrepeat",       "--randrepeat");
    $self->addValueOption(\@args,  "softrandommap",    "--softrandommap");
    $self->addValueOption(\@args,  "debug",            "--debug");
    $self->addValueOption(\@args,  "useOsRand",        "--use_os_rand");
    $self->addSimpleOption(\@args, "group_reporting",  "--group_reporting");
    if ($self->{latency} == 0) {
      $self->addValueOption(\@args, "gtod_reduce",     "--gtod_reduce");
    }
    $self->addValueOption(\@args,  "bufferPattern",    "--buffer_pattern");
    $self->addValueOption(\@args,  "cleanupBenchmark", "--unlink");
    $self->addValueOption(\@args,  "directIo",         "--direct");
    $self->addValueOption(\@args,  "rate",             "--rate");
    $self->addValueOption(\@args,  "runTime",          "--runtime");
    $self->addValueOption(\@args,  "rwmixread",        "--rwmixread");
    $self->addValueOption(\@args,  "cpusAllowed",      "--cpus_allowed");
    $self->addValueOption(\@args,  "ioDepth",          "--iodepth");
    $self->addValueOption(\@args,
                          "ioDepthBatchComplete",
                          "--iodepth_batch_complete");
    $self->addValueOption(\@args,
                          "ioDepthBatchSubmit",
                          "--iodepth_batch_submit");
    $self->addValueOption(\@args,  "ioEngine",         "--ioengine");
    $self->addValueOption(\@args,  "ioScheduler",      "--ioscheduler");
    $self->addValueOption(\@args,  "ioSize",           "--io_size");
    $self->addValueOption(\@args,  "loops",            "--loops");
    $self->addValueOption(\@args,  "lockMem",          "--lockmem");
    $self->addValueOption(\@args,  "scrambleBuffers",  "--scramble_buffers");
    $self->addValueOption(\@args,  "fallocate",        "--fallocate");
    $self->addValueOption(\@args,  "albGenStream",     "--albgenstream");
    $self->addValueOption(\@args,  "offset",           "--offset");
    $self->addValueOption(\@args,  "offsetIncrement",  "--offset_increment");
    $self->addValueOption(\@args,  "fsync",            "--fsync");
    $self->addValueOption(\@args,  "endFsync",         "--end_fsync");
    $self->addValueOption(\@args,
                          "compressPercent",
                          "--buffer_compress_percentage");
    if (defined($self->{compressPercent})) {
      $self->addValueOption(\@args, "blockSize", "--buffer_compress_chunk");
    }

    # Verify always needs two options, the verify method and to actually
    # do the verify.
    $self->addValueOption(\@args,  "verify",        "--verify");
    if (defined($self->{verify})) {
      push(@args, "--do_verify=1");
    }
    $self->addValueOption(\@args,  "verifyBacklog", "--verify_backlog");
    $self->addValueOption(\@args,  "verifyDump",    "--verify_dump");
    $self->addValueOption(\@args,  "verifyFatal",   "--verify_fatal");
  }
  return @args;
}

1;
