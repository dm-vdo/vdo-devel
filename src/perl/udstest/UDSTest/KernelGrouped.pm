##
# Base class for tests which run kernel mode linux unit tests.  Runs all the
# tests quickly grouped into a single testcase.
#
# $Id$
##
package UDSTest::KernelGrouped;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertFalse
  assertNumArgs
  assertRegexpDoesNotMatch
);
use Permabit::KernelModule;
use Permabit::LabUtils qw(getTestBlockDeviceName);
use Permabit::Utils qw(makeFullPath);

use base qw(UDSTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %PROPERTIES
  = (
     # @ple The test arguments
     arguments         => "",
     # @ple The initial setting of UDS_LOG_LEVEL
     logLevel          => undef,
     # @ple The glob pattern of unit test names to run
     unitTestName      => "*_[ntx]*",
    );

#############################################################################
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  # restartNeeded indicates that the last test failed.
  # restartWanted indicates that we know the host will not pass checkServer.
  # In either case we will need to restart the host now.
  $self->{_restartNeeded} ||= $self->{_restartWanted};
  $self->_possiblyRestartMachine();
  my $machine = $self->getUserMachine();
  $self->runTearDownStep(sub { $machine->runSystemCmd("sudo losetup -D"); });
  if (defined($self->{_module})) {
    my $kernelLogCursor = $machine ? $machine->getKernelJournalCursor() : 0;
    $self->runTearDownStep(sub { $self->{_module}->unload(); });
    delete($self->{_module});
    if ($machine) {
      my $checkMessages
        = sub {
          my $messages = $machine->getKernelJournalSince($kernelLogCursor);
          assertRegexpDoesNotMatch(qr/assertion/, $messages);
        };
      $self->runTearDownStep($checkMessages);
    }
  }
  $self->SUPER::tear_down();
}

#############################################################################
##
sub testRunner {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getUserMachine();
  $self->_setupMachineForTest();
  my $udsDir = "src/c++/uds";
  my $modDir = makeFullPath($self->{topDir}, $udsDir, "kernelLinux/tests");
  $self->{_module}
    = Permabit::KernelModule->new(machine    => $machine,
                                  modDir     => $modDir,
                                  modName    => "zubenelgenubi",
                                  modVersion => 1,);
  $self->{_module}->load();

  # unitTestName is either the name of a test, or a glob pattern we can
  # use to generate the list of tests.  Generate the list.
  my $moduleDir = "/sys/zubenelgenubi";
  $machine->runSystemCmd("(cd $moduleDir; ls -d $self->{unitTestName})");
  my @tests = split(/\s+/, $machine->getStdout());

  # Run each test.  Any test failure will be logged at FATAL level.  After all
  # the tests have run, we will produce a FAILURE message naming each test that
  # does not pass.
  my @failures;
  my @softLockups;
  my $lastError;
  foreach my $test (@tests) {
    $self->_possiblyRestartMachine();
    $log->info("Test $test");
    # Set optional UDS parameters
    if (defined($self->{logLevel})) {
      $machine->setProcFile($self->{logLevel}, "/sys/uds/parameter/log_level");
    }

    # Run the next test
    my $kernelLogCursor = $machine->getKernelJournalCursor();
    eval {
      $machine->runSystemCmd("echo $self->{arguments}"
                             . " | sudo dd of=/sys/zubenelgenubi/$test/run");
      $log->info("Elapsed:\n"
                 . $machine->cat("/sys/zubenelgenubi/$test/elapsed"));
      $log->info("Results:\n"
                 . $machine->cat("/sys/zubenelgenubi/$test/results"));
      my $messages = $machine->getKernelJournalSince($kernelLogCursor);
      $log->info("kernel messages:\n" . $messages);
      assertFalse($machine->catAndChomp("/sys/zubenelgenubi/$test/failed"),
                  "$test failed");
      assertRegexpDoesNotMatch(qr/assertion.*failed/, $messages);
      if ($messages =~ qr/watchdog: BUG: soft lockup/) {
        push(@softLockups, $test);
        # The host is functional but does not pass checkServer.
        $self->{_restartWanted} = 1;
      }
    };
    if ($EVAL_ERROR) {
      my $testError = $EVAL_ERROR;
      # The test failed.  Log and record the failure.
      $lastError = "$test: $testError";
      $log->fatal($testError);
      push(@failures, $test);
      # The host needs to restart.
      $self->{_restartNeeded} = 1;
      if (ref($testError) && $testError->isa("Permabit::Exception::Signal")) {
        # XXX Get kernel stack traces to help diagnose ALB-2960
        $machine->setProcFile("t", "/proc/sysrq-trigger");
        # We took a signal and must stop running tests
        last;
      }
    }
  }

  if (scalar(@failures) > 0) {
    if (scalar(@softLockups) > 0) {
      $log->warn(join(" ", "Soft lockups:", @softLockups));
    }
    if (scalar(@failures) == 1) {
      die($lastError);
    }
    die(join(" ", @failures));
  }
  if (scalar(@softLockups) > 0) {
    die(join(" ", "Soft lockups:", @softLockups));
  }
}

#############################################################################
# Prepare the host for the test.  This is called once when the test starts, and
# again if we ever need to reboot the host.
##
sub _setupMachineForTest {
  my ($self) = assertNumArgs(1, @_);
  my $machine = $self->getUserMachine();
  # Make sure /dev/zubenelgenubi_scratch is a block device.  We will use this
  # device for testing.  If it is not a block device, create it as a symbolic
  # link to the first suitable device.
  my $zubScratch = "/dev/zubenelgenubi_scratch";
  if ($machine->sendCommand("test -b $zubScratch") != 0) {
    my $deviceName = getTestBlockDeviceName($machine);
    $machine->runSystemCmd("sudo ln -f -s $deviceName $zubScratch");
  }

  # Set up loop devices for multi-device tests.
  foreach my $i (0 .. 1) {
    my $multiScratch = "/dev/zubenelgenubi_scratch-$i";
    # Make this size large enough that multiple devices won't overlap.
    my $size = 12;
    my $offset = $i * $size;
    $machine->runSystemCmd("sudo losetup -f --show -o ${offset}GB "
                           . "--sizelimit ${size}GB $zubScratch");
    my $loopName = $machine->getStdout();
    chomp($loopName);
    if ($machine->sendCommand("test -b $multiScratch") != 0) {
      $machine->runSystemCmd("sudo ln -f -s $loopName $multiScratch");
    }
  }

  # XXX Log the current state of the kernel page allocator.  For ALB-2897.
  $machine->executeCommand("cat /proc/buddyinfo");
}

#############################################################################
# Reboot the machine after a test failure.
##
sub _possiblyRestartMachine {
  my ($self) = assertNumArgs(1, @_);
  if ($self->{_restartNeeded}) {
    delete($self->{_restartNeeded});
    delete($self->{_restartWanted});
    # We may have leaked memory, so we reboot the host to get back to a clean
    # state.  And we may have crashed the kernel, so we use the big hammer.
    $self->getUserMachine()->emergencyRestart();
    # Reload the zubenelgenubi kernel module.
    $self->{_module}->reload();
    # Recreate the test devices and symbolic links.
    $self->_setupMachineForTest();
  }
}

1;
