###############################################################################
# A RemoteMachine with support for running testing related commands.
#
# These commands include:
#    fsync           Test program to sync a file or directory
#    genDataSet.py   Test program to generate a random data set
#    genDiscard      Test program to do a BLKDISCARD ioctl
#    murmur3collide  Test program to generate murmur3 hash collisions
#
# @description
#
# $Id$
##
package Permabit::UserMachine;

use strict;
use warnings FATAL => qw(all);
use Carp;
use English qw(-no_match_vars);
use File::Path;
use Log::Log4perl;
use Storable qw(dclone);

use File::Basename qw(basename);
use Permabit::Assertions qw(
  assertDefined
  assertEqualNumeric
  assertLTNumeric
  assertMinArgs
  assertMinMaxArgs
  assertNe
  assertNumArgs
  assertOptionalArgs
  assertRegexpMatches
  assertTrue
);
use Permabit::Constants;
use Permabit::LabUtils qw(getTestBlockDeviceNames isVirtualMachine);
use Permabit::PlatformUtils qw(isMaipo);
use Permabit::SystemUtils qw(runCommand);
use Permabit::Utils qw(hashExtractor makeFullPath);
use Permabit::Version qw($VDO_MODNAME);

use base qw(Permabit::RemoteMachine);

# Overload stringification to print something meaningful
use overload q("") => \&as_string;

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $GENDATA_BASE_REGEXP =
  '(\d+) files; (\d+) bytes at (\d+\.\d+(E[+-]\d+)?) files/sec; (\d+\.\d+(E[+-]\d+)?) MB/sec';
my $GENDATA_WRITE_REGEXP = "Wrote $GENDATA_BASE_REGEXP";
my $GENDATA_SEED_REGEXP = 'gen.randseed=(\d+)';

# Locations of executables copied to nfsShareDir by Permabit::FileCopier.
my $FSYNC          = "executables/fsync";
my $GEN_DATA_SET   = "src/python/bin/genDataSet.py";
my $GEN_DISCARD    = "src/c++/vdo/bin/genDiscard";
my $MURMUR3COLLIDE = "src/c++/vdo/bin/murmur3collide";

# Map of kernel-error labels to regular expressions or code that will match
# them.  Keep the patterns in sync with the VDO source (files noted below) and
# the Linux kernel.
my %KERNEL_LOG_ERROR_TABLE
  = (
     # permassert.c
     assert     => [qr/(uds|$VDO_MODNAME).*: assertion ".*" .* failed/,
                    "assertion failure"],
     # kernel
     ata        => [qr/: failed command: READ FPDMA QUEUED/,
                    "ATA read error"],
     # kernel
     blocked    => [\&_taskBlocked,
                    "Some I/O tasks were blocked for too long:"],
     # kernel
     mapError   => [qr/mapToSystemError: mapping errno/,
                    "Error path with incorrect value"],
     # kernel
     rcuStall   => [qr/self-detected stall on CPU/,
                    "RCU stall"],
     # readOnlyNotifier.c
     readonly   => [qr/entering read-only mode/,
                    "Read only mode"],
     # vdoLoad.c
     rebuild    => [qr/[Rr]ebuilding reference counts/,
                    "Unexpected VDO rebuild:"],
     # kernel
     softLockup => [qr/watchdog: BUG: soft lockup /,
                    "Soft lockup"],
     # kernel
     sysfs      => [qr/fill_read_buffer: .* returned bad count/,
                    "Possible sysfs buffer overrun"],
    );

# The _taskBlocked method uses this table to identify process names that are ok
# to block for a long time.
my %TASKS_TO_IGNORE
  = (
     # cron runs these programs at I/O priority set to "idle", which means that
     # an I/O intensive workload can totally lock them out.  Longer names get
     # truncated to 14 characters.
     dlocate          => 1,
     "readahead.cron" => 1,
     "update-dlocate" => 1,
    );

##
# @paramList{new}
my %PROPERTIES
  = (
     # @ple The directory test executables are found.  This must be set to use
     #      fsync, genDataSet, genDiscard or murmur3collide.
     nfsShareDir        => undef,
     # @ple The raw devices used to back test devices
     rawDeviceNames     => undef,
     # @ple Whether to save the scratch data when saving logs
     saveScratchData    => 0,
     # @ple The directory to place scratch files in
     scratchDir         => undef,
     # @ple The directory to put the user tool binaries in
     userBinaryDir      => undef,
     # @ple The directory to place logfiles in
     workDir            => undef,
     # @ple running cursor into the journal log for kernel messages
     _kernelLogCursor   => undef,
     # @ple kernel log errors found since the last reboot
     _kernLogErrorFound => undef,
     # @ple kernel log errors to check for
     _kernLogErrors     => [ keys(%KERNEL_LOG_ERROR_TABLE) ],
     # @ple running cursor into the journal log
     _logCursor         => undef,
     # @ple the path to the python site library
     _pythonLibraryPath => undef,
    );
##

###############################################################################
# Create a new UserMachine object.  This will create an IPC::Session
# connection to the remote server.
#
# @params{new}
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my %args = hashExtractor({ %{ dclone(\%PROPERTIES) }, @_ },
                           getParameters());
  my $self = $class->SUPER::new(%args);

  # Ensure that scratchDir and workDir exist on the host.
  assertDefined($self->{scratchDir}, "scratchDir not defined");
  assertDefined($self->{workDir}, "workDir not defined");
  $self->runSystemCmd("mkdir -p $self->{workDir} $self->{scratchDir}");

  # Ensure that userBinaryDir exists on the host, when defined.
  # The userBinaryDir property will not be defined in cases where tests using UserMachine do
  # not go through VDOTest.pm.
  if (defined($self->{userBinaryDir})) {
    $self->runSystemCmd("mkdir -p $self->{userBinaryDir}");
  }

  if (isVirtualMachine($self->getName())) {
    $self->removeKernelLogErrorCheck("blocked");
  }

  # In testing, we want to hear about hung tasks.  Make sure that previous
  # testing hasn't used up all the reporting slots.
  $self->setHungTaskWarnings();

  return $self;
}

###############################################################################
# @inherit
##
sub getParameters {
  assertNumArgs(0, @_);
  return [
          keys(%PROPERTIES),
          @{Permabit::RemoteMachine::getParameters()},
         ];
}

###############################################################################
# @inherit
##
sub getLogFileList {
  my ($self) = assertNumArgs(1, @_);

  return (
          $self->{workDir},
          $self->{saveScratchData} ? $self->{scratchDir} : (),
         );
}

###############################################################################
# @inherit
##
sub getCleanupFiles {
  my ($self) = assertNumArgs(1, @_);
  return ($self->{scratchDir}, $self->{workDir});
}

###############################################################################
# Return the full path to the directory that log files are placed in
#
# @return The directory that log files are placed in
##
sub getWorkDir {
  my ($self) = assertNumArgs(1, @_);
  return $self->{workDir};
}

###############################################################################
# Return the full path to the directory that scratch files are placed in
#
# @return The directory that scratch files are placed in
##
sub getScratchDir {
  my ($self) = assertNumArgs(1, @_);
  return $self->{scratchDir};
}

###############################################################################
# Return the system python site path
#
# @return  a string containing the default python site path
##
sub getPythonLibraryPath {
  my ($self) = assertNumArgs(1, @_);
  if (!defined($self->{_pythonLibraryPath})) {
    my $pythonCmd = "from distutils.sysconfig import get_python_lib;"
      . " print(get_python_lib())";

    # Each platform has its own "correct" python for program invocation.
    # RHEL8 has platform-python, Fedora 29 has system-python, and Fedora 30+
    # has python3. If something new comes along, hopefully it has at least
    # one of these.
    my $pythonPath = "/usr/libexec/platform-python";
    if (!$self->pathExists($pythonPath)) {
      $pythonPath = "/usr/libexec/system-python";
    }
    if (!$self->pathExists($pythonPath)) {
      $pythonPath = "/usr/bin/python3";
    }
    assertTrue($self->pathExists($pythonPath));

    $self->runSystemCmd("$pythonPath -c '$pythonCmd'");
    my $stdout = $self->getStdout();
    chomp($stdout);
    my $re = 'python[0-9]\.[0-9]*/site-packages$';
    assertRegexpMatches(qr/$re/, $stdout);
    $self->{_pythonLibraryPath} = $stdout;
  }
  return $self->{_pythonLibraryPath};
}

#############################################################################
# Find the requested executable locally.
#
# @param executable The name of the executable to look for
#
# @return The path to the executable or undef
##
sub findNamedExecutable {
  my ($self, $executable) = assertNumArgs(2, @_);
  my $searchPath = makeFullPath($self->{userBinaryDir}, $executable);
  my $result = runCommand($self->getName(), "test -x $searchPath");

  if ($result->{returnValue} != 1) {
    $self->{_executables}->{$executable} = $searchPath;
    return $searchPath;
  }

  return undef;
}

#############################################################################
# Select the default devices to use for tests on the indicated machine.
#
# @return the arrayref of raw devices to use
##
sub selectDefaultRawDevices {
  my ($self) = assertNumArgs(1, @_);
  $log->info("Selecting raw devices"); #
  if (!defined($self->{rawDeviceNames})) {
    $self->{rawDeviceNames} = getTestBlockDeviceNames($self);
    $self->{rawDeviceNames}
      = [ map { s|^/dev/||r } @{$self->{rawDeviceNames}} ];
    $log->debug("rawDeviceNames = " . join(' ', @{$self->{rawDeviceNames}}));
  }

  return $self->{rawDeviceNames};
}

###############################################################################
# Check the log for kernel errors and advance the cursor
##
sub checkForKernelLogErrors {
  my ($self) = assertNumArgs(1, @_);
  my @lines = split("\n", $self->getNewKernelJournal());
  my @failStrings = ();
  foreach my $error (@{$self->{_kernLogErrors}}) {
    my ($matcher, $description) = @{$KERNEL_LOG_ERROR_TABLE{$error}};
    my @errorLines;
    if (ref($matcher) eq "CODE") {
      @errorLines = grep { $matcher->($_) } @lines;
    } elsif (ref($matcher) eq "Regexp") {
      @errorLines = grep { $_ =~ $matcher } @lines;
    } else {
      confess("$matcher cannot be used to match errors");
    }
    if (scalar(@errorLines) > 0) {
      $self->{_kernLogErrorFound}{$error} = 1;
      push(@failStrings, $description);
      $log->fatal(join("\n", $description, @errorLines));
    }
  }
  if (scalar(@failStrings) > 0) {
    confess("Found errors in kernel log: " . join(", ", @failStrings));
  }
}

###############################################################################
# Does a clean restart.
#
# The real restart is done in our superclass, but we need to clear the record
# of kernel log failures seen since a restart was done.
##
sub restart {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::restart();
  # Now that we are restarted, clear the table of kernel log errors we have
  # seen
  delete($self->{_kernLogErrorFound});
}

###############################################################################
# Close the machine, but first do a clean restart if we have recorded any
# kernel log error that will prevent RSVP from releasing the machine.
#
# The particular error that we do this for is a soft lockup.  We will have
# already failed the test, and have already grabbed logs containing the data
# we need to diagnose the problem.  We do not need to move the machine into
# maintenance.
##
sub closeForRelease {
  my ($self) = assertNumArgs(1, @_);
  $self->runCleanupSteps();
  if ($self->{_kernLogErrorFound}->{softLockup}) {
    $self->restart();
  }
  $self->close();
}

###############################################################################
# Run the fsync executable to sync a file or directory.
#
# @param args  fsync command line arguments
##
sub fsync {
  my ($self, @args) = assertMinArgs(2, @_);
  my $fsync = $self->makeNfsSharePath($FSYNC);
  $self->runSystemCmd(join(" ", "sudo", $fsync, @args));
}

###############################################################################
# Run the genDiscard executable to send a trim request to a block device.
# Takes arguments as key-value pairs.
#
# @oparam bs     Block size (passed using --bs= to genDiscard)
# @oparam count  Block count (passed using --count= to genDiscard)
# @oparam of     Device path (passed using --of= to genDiscard)
# @oparam seek   The first block number to trim on the device (passed using
#                --seek= to genDiscard)
##
sub genDiscard {
  my ($self, %params) = assertMinArgs(1, @_);
  my $command = join(" ", "sudo", $self->makeNfsSharePath($GEN_DISCARD),
                     map { "--$_=$params{$_}" } keys(%params));
  $self->runSystemCmd($command);
  $log->debug($self->getStdout());
}

###############################################################################
# Make the full path name of a file that is in the nfs shared executable
# directory.
#
# @param $path  Partial path name starting at nfsShareDir
#
# @return the full path name
##
sub makeNfsSharePath {
  my ($self, $path) = assertNumArgs(2, @_);
  return makeFullPath($self->{nfsShareDir}, $path);
}

###############################################################################
# Run the murmur3collide executable to copy blocks that are modified to
# create murmur3 hash collisions.  Takes arguments as key-value pairs.
#
# @param  if      Input path (passed using --if= to murmur3collide)
# @param  of      Output path (passed using --of= to murmur3collide)
# @oparam bs      Block size (passed using --bs= to murmur3collide)
# @oparam count   Block count (passed using --count= to murmur3collide)
# @oparam fsync   Use fsync before closing the output file
# @oparam seek    The first block number to write on the output file (passed
#                 using --skip= to murmur3collide)
# @oparam skip    The first block number to read on the input file (passed
#                 using --seek= to murmur3collide)
# @oparam verify  Verify the output file (instead of writing it)
##
sub murmur3collide {
  my ($self, %params) = assertMinArgs(5, @_);
  my @NO_VALUE_OPTIONS = qw(fsync verify);
  my @noValue = map { "--$_" } grep { $params{$_} } @NO_VALUE_OPTIONS;
  map { delete($params{$_}) } @NO_VALUE_OPTIONS;
  my $command = join(" ", "sudo", $self->makeNfsSharePath($MURMUR3COLLIDE),
                     @noValue, map { "--$_=$params{$_}" } keys(%params));
  $self->runSystemCmd($command);
}

###############################################################################
# Generate a dataset using genDataSet.  Properties in the provided hashref
# will be passed to genDataSet.
#
# @param props  A hashref of properties to be passed to GenRandomDirTree.
#               If not provided, the "gen.root.dir" and "gen.randseed"
#               properties will be filled in upon return.
#
# @return A hashref of statistics of bytes written or deleted.
##
sub generateDataSet {
  my ($self, $props) = assertNumArgs( 2, @_);
  $props->{'gen.root.dir'} ||= $self->{scratchDir};
  my $output = $self->_runGenDataSet($props, "");
  my $stats = $self->_makeGenDataSetStat($output, $props);
  $props->{"gen.randseed"} = $stats->{seed};
  return $stats;
}

###############################################################################
# Get the current size of the specified file
#
# @param file   The name of the file
#
# @return the current byte count
##
sub getFileSize {
  my ($self, $file) = assertNumArgs(2, @_);
  $self->runSystemCmd("sudo stat -c '\%s' $file");
  my $stdout = $self->getStdout();
  return 0 + $stdout;
}

###############################################################################
# Change the list kernel log error checks in the middle of a test run.
#
# @oparam add  A key into %KERNEL_LOG_ERROR_TABLE,
#              or listref to a list of such keys
# @oparam del  A key into %KERNEL_LOG_ERROR_TABLE,
#              or listref to a list of such keys
##
sub changeKernelLogErrorChecks {
  my %OPTIONS = (
                 add => [],
                 del => [],
                );
  my ($self, $options) = assertOptionalArgs(1, \%OPTIONS, @_);
  # Check for kernel log errors using the current settings
  $self->checkForKernelLogErrors();
  # Change the settings
  my @errors = @{$self->{_kernLogErrors}};
  my $del = $options->{del};
  foreach my $key (ref($del) ? @$del : ($del)) {
    assertTrue(exists($KERNEL_LOG_ERROR_TABLE{$key}));
    @errors = grep { $_ ne $key } @errors;
  }
  my $add = $options->{add};
  foreach my $key (ref($add) ? @$add : ($add)) {
    assertTrue(exists($KERNEL_LOG_ERROR_TABLE{$key}));
    if (scalar(grep { $_ eq $key } @errors) == 0) {
      push(@errors, $key);
    }
  }
  $self->{_kernLogErrors} = \@errors;
}

###############################################################################
# Remove an error type from the list of errors to check in the kernel log.
#
# @param errorType A key into %KERNEL_LOG_ERROR_TABLE
##
sub removeKernelLogErrorCheck {
  my ($self, $errorType) = assertNumArgs(2, @_);
  assertTrue(exists($KERNEL_LOG_ERROR_TABLE{$errorType}));
  my @errors = grep { $_ ne $errorType } @{$self->{_kernLogErrors}};
  $self->{_kernLogErrors} = [@errors];
}

###############################################################################
# Run a piece of code with a particular kernel log check disabled.
#
# @param code       The code to run
# @param errorType  A key into %KERNEL_LOG_ERROR_TABLE
##
sub withKernelLogErrorCheckDisabled {
  my ($self, $code, $errorType) = assertNumArgs(3, @_);
  # Check for kernel log errors using the active settings
  $self->checkForKernelLogErrors();
  local $self->{_kernLogErrors} = [@{$self->{_kernLogErrors}}];
  $self->removeKernelLogErrorCheck($errorType);
  $code->();
  # Check for kernel log errors with the modified settings
  $self->checkForKernelLogErrors();
  # We now return to the original settings (even if something croaked)
}

###############################################################################
# Run GenRandomDirTree.
#
# @param  props      A hashref or properties to pass as arguments to gendataset
# @param  arg        Extra argument
#
# @return The standard output.
##
sub _runGenDataSet {
  my ($self, $props, $arg) = assertNumArgs(3, @_);
  my $gen = $self->makeNfsSharePath($GEN_DATA_SET);
  my $options = "--stdout --logDir $self->{workDir}";
  while (my ($key, $value) = each %{$props}) {
    $options .= " -D$key=$value";
  }
  $self->runSystemCmd("$gen $options $arg");
  my $output = $self->getStdout();
  $log->debug("genDataSet complete, STDOUT: $output");
  return $output;
}

###############################################################################
# Create GenRandomDirTree statistics.
#
# @param s          The output from genDataSet
# @param props      The properties used for running genDataSet.
#
# @return A hashref describing statistics for the generated data set.
##
sub _makeGenDataSetStat {
  my ($self, $s, $props) = assertNumArgs(3, @_);

  my ($actualSeed) = $s =~ m|$GENDATA_SEED_REGEXP|;
  my ($files, $bytes, $fps, $ign, $mbps) = $s =~ m|$GENDATA_WRITE_REGEXP|;
  return {
          dest     => $props->{"gen.root.dir"},
          seed     => $actualSeed,
          files    => $files,
          bytes    => $bytes,
          fps      => $fps,
          mbps     => $mbps,
          hostname => $self->getName(),
         };
}

###############################################################################
# This is the code for the "blocked" entry in %KERNEL_LOG_ERROR_TABLE
##
sub _taskBlocked {
  my ($line) = assertNumArgs(1, @_);
  if ($line =~ qr/INFO: task (\S+):\d+ blocked for more than \d+ seconds/) {
    return !exists($TASKS_TO_IGNORE{$1});
  }
  return 0;
}

###############################################################################
# Get the process IDs, if any, under which the specified program is running.
#
# @param program  The program name to look for on the remote machine
#
# @return  a possibly-empty list of strings
##
sub getProgramPIDs {
  my ($self, $program) = assertNumArgs(2, @_);
  $self->sendCommand("pidof $program");
  my $status = $self->getStatus();
  my @pidStrings = ();
  if ($status == 0) {
    my $result = $self->getStdout();
    chomp($result);
    @pidStrings = split(" ", $result);
    assertLTNumeric(0, scalar(@pidStrings));
  } else {
    # Exit status 1 means no process found; other status values are
    # undefined.
    assertEqualNumeric(1, $status, "error running pidof $program");
  }
  return @pidStrings;
}

#############################################################################
# Fetch the device mapper table for all devices.
#
# @return  a list of strings, one per output line
##
sub deviceMapperTable {
  my ($self) = assertNumArgs(1, @_);
  $self->runSystemCmd("sudo dmsetup table");
  my $output = $self->getStdout();
  chomp($output);
  return split(/\n/, $output);
}

#############################################################################
# Translate one line of vgs output to a collection of attribute names and
# values. The vgs output line supplied must use the "LVM2_FOO=bar" syntax, as
# generated by the "--nameprefixes" and "--unquoted" options.
#
# This is the text-parsing step of volumeGroupInfo, below.
#
# @param line  The vgs output line, with or without leading whitespace and
#              trailing newline
#
# @return  a hash ref mapping attribute names (without the LVM2_ prefix) to
#          (string) values
##
sub _vgsLineToHash {
  my ($line) = assertNumArgs(1, @_);
  chomp($line);
  $line =~ s/^ +//;
  my @fields = split(/ /, $line);
  # Each field is "LVM2_FOOBAR=stuff", though "stuff" may be empty.
  my %results = map {
    my ($key, $value) = split(/=/, $_);
    $key =~ s/^LVM2_//;
    $key => $value;
  } @fields;
  return \%results;
}

#############################################################################
# Fetch information for a volume group.
#
# @param vgName  The volume group name
#
# @return  a hashref of attributes and values if the volume group exists;
#          undef if it doesn't exist
#
# @croaks if the command fails but not with a volume-group-not-found message
##
sub volumeGroupInfo {
  my ($self, $vgName) = assertNumArgs(2, @_);
  # Generate regular and precise output for machine post-processing.
  # This may need to change if a case comes up where the quoting is needed.
  my $vgsOptions
    = "--nameprefixes --noheadings -o vg_all --unquoted --units b";

  $self->sendCommand("sudo vgs $vgsOptions $vgName");
  if ($self->getStatus() != 0) {
    assertRegexpMatches(qr/Volume group ".*" not found/, $self->getStderr());
    return undef;
  }
  return _vgsLineToHash($self->getStdout());
}

###############################################################################
# Chase symlinks and return the true pathname for the file or device
# supplied.
#
# @param path  a path name which may identify a symlink
#
# @return  the canonical name
##
sub resolveSymlink {
  my ($self, $path) = assertNumArgs(2, @_);
  my $errno = $self->sendCommand("readlink -f $path");
  assertEqualNumeric(0, $errno);
  my $resolvedPath = $self->getStdout();
  chomp($resolvedPath);
  assertNe('', $resolvedPath, "couldn't resolve path: $path");
  if ($resolvedPath ne $path) {
    $log->debug("resolved symlink $path -> $resolvedPath");
  }
  return $resolvedPath;
}

########################################################################
# Check if lvmdevices is available and the devices file is enabled. Older
# versions of LVM do not support the devices file.
#
# @return true if lvmdevices can be used, false otherwise
##
sub isLvmdevicesAvailable {
  my ($self) = assertNumArgs(1, @_);

  if ($self->sendCommand("sudo lvmdevices") != 0) {
    if ($self->getStderr() =~ m/Devices file not enabled/m) {
      return 0;
    }
  }
  return 1;
}

########################################################################
# Check if this device is present in the LVM devices file
#
# @param devicePath  The path of the device to check
#
# @return true if the device is found in the devices file, false otherwise
##
sub isInLVMDevicesFile {
  my ($self, $devicePath) = assertNumArgs(2, @_);

  if (!$self->isLvmdevicesAvailable()) {
    return 0;
  }

  $self->runSystemCmd("sudo lvmdevices");
  my $result = $self->getStdout();
  my @devices = ( $result =~ /.*DEVNAME=([^\s]*)/g );
  foreach my $device (@devices) {
    my $resolved;
    eval {
      $resolved = $self->resolveSymlink($device);
    };
    if ($EVAL_ERROR) {
      $log->info("Couldn't resolve symlink for device $device ... skipping");
      next;
    }
    if (($devicePath eq $resolved) || ($devicePath eq $device)) {
      return 1;
    }
  }
  return 0;
}

########################################################################
# Parse LVM devices output and return an array of valid devices.
#
# This function runs 'sudo lvmdevices', parses the output for DEVNAME entries,
# and uses eval to safely skip devices that can't be resolved. This can happen
# if the device is still in the devices file but doesn't exist anymore, or if
# the lvm device is not currently enabled.
#
# @return An array of device info hashes, each containing:
#         - deviceName: the original device name from the devices file
#         - resolvedName: the resolved device path (if available)
##
sub getValidDevicesInLVMDevicesFile {
  my ($self) = assertNumArgs(1, @_);
  my @validDevices = ();

  if (!$self->isLvmdevicesAvailable()) {
    return @validDevices;
  }

  $self->runSystemCmd("sudo lvmdevices");
  my $result = $self->getStdout();
  my @devices = ( $result =~ /.*DEVNAME=([^\s]*)/g );
  foreach my $device (@devices) {
    my $resolved;
    eval {
      $resolved = $self->resolveSymlink($device);
    };
    if ($EVAL_ERROR) {
      $log->info("Couldn't resolve symlink for device $device ... skipping");
      next;
    }
    push(@validDevices, { deviceName => $device, resolvedName => $resolved, });
  }
  return @validDevices;
}

########################################################################
# This function runs 'sudo lvmdevices --check' and parses the output for DEVNAME entries.
#
# @return An array of device names that are not valid
##
sub getInvalidDevicesInLVMDevicesFile {
  my ($self) = assertNumArgs(1, @_);
  my @invalidDevices = ();

  if (!$self->isLvmdevicesAvailable()) {
    return @invalidDevices;
  }

  $self->runSystemCmd("sudo lvmdevices --check");
  my $result = $self->getStdout();
  @invalidDevices = ( $result =~ /.*DEVNAME=([^\s]*)/g );
  return @invalidDevices;
}

########################################################################
# Clean up invalid entries in the LVM devices file for this machine.
# This method finds devices that are marked as invalid by lvmdevices --check
# and removes them from the devices file.
##
sub cleanupLVMDevicesFile {
  my ($self) = assertNumArgs(1, @_);

  my $machineName = $self->getName();
  $log->info("Cleaning up LVM devices file on $machineName");

  # Check for bad devices
  my @invalidDevices = $self->getInvalidDevicesInLVMDevicesFile();
  foreach my $deviceName (@invalidDevices) {
    # Escape the device path for sed command (same logic as in BlockDevice.pm)
    my $escapedDevicePath = $deviceName;
    $escapedDevicePath =~ s/([\/\.])/\\$1/g;  # Escape forward slashes and dots

    $self->sendCommand("sudo sed -i '/DEVNAME=$escapedDevicePath\\b/d' "
                      . "/etc/lvm/devices/system.devices 2>/dev/null");
  }
  my @finalDevices = $self->getValidDevicesInLVMDevicesFile();
  my @finalDeviceNames = map { $_->{deviceName} } @finalDevices;
  $log->info("Final LVM devices on $machineName: " . join(", ", @finalDeviceNames));
}

###############################################################################
# Overload default stringification to print our hostname
##
sub as_string {
  my $self = shift;
  return "UserMachine($self->{hostname})";
}

1;
