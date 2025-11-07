##
# Utility functions for manipulating Linux Kernels
#
# $Id$
##
package Permabit::KernelUtils;

use strict;
use warnings FATAL => qw(all);
use Carp qw(confess croak);
use English qw(-no_match_vars);
use List::Util qw(max min);
use Log::Log4perl;
use Math::BigInt;
use Permabit::Assertions qw(
  assertEq
  assertFalse
  assertGENumeric
  assertNear
  assertNumArgs
  assertTrue
);
use Permabit::Constants;
use Permabit::Grub;
use Permabit::LabUtils qw(
  getTotalRAM
  isVirtualMachine
  rebootMachines
  setHungTaskTimeout
);
use Permabit::PlatformUtils qw(getDistroInfo isFedora);
use Permabit::RemoteMachine;
use Permabit::SystemUtils qw(
  assertCommand
  assertQuietCommand
  getScamVar
);
use Permabit::Utils qw(sizeToText);

use base qw(Exporter);

our @EXPORT_OK = qw (
  getAddressableMemory
  setupKernelMemoryLimiting
  removeKernelMemoryLimiting
  setupRawhideKernel
  removeRawhideKernel
);

our $VERSION = 1.0;

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# Find the amount of actual RAM the kernel currently believes exists.
#
# @param host   The host in question (will be rebooted)
#
# @return  the amount of memory currently addressable by the kernel
##
sub getAddressableMemory {
  my ($host) = assertNumArgs(1, @_);
  my $machine = Permabit::RemoteMachine->new(hostname => $host);
  my $preRebootCursor = $machine->getKernelJournalCursor();

  $machine->restart();
  my $logText = $machine->getKernelJournalSince($preRebootCursor);
  # Match all the following types of lines:
  # Aug 19 05:33:26 localhost kernel: Memory: 1662452k/1781760k available...
  # Aug 19 07:50:12 localhost kernel: [    0.000000] Memory: 1712720k/1789952k available...
  # Aug 19 07:50:12 localhost kernel: [    0.000000] Memory: 1712720K/1789952K available...
  if ($logText !~ qr/ Memory: \d+[Kk]\/(\d+)[Kk] available/) {
    $log->error("Couldn't find a memory line in:\n $logText");
    confess("Failed to find Memory line")
  }

  return $1 * $KB;
}

#############################################################################
# Check whether a value is within a specified tolerance of a reference value.
#
# @param expected   The expected value
# @param actual     The actual value
# @param tolerance  The tolerance permitted (can be a percent)
#
# @return true if the actual value is within tolerance of the expected value
##
sub _isNear {
  my ($expected, $actual, $tolerance) = assertNumArgs(3, @_);
  if ($tolerance =~ s/%$//) {
    $tolerance = $expected * ($tolerance / 100.0);
  }
  return (abs($expected - $actual) < $tolerance);
}

#############################################################################
# Parse /proc/iomem content of the specified host, returning a sorted
# list of memory ranges.
#
# Written in part by Claude Sonnet 4.
#
# @param host   The host to examine
#
# @return an array ref of sorted memory (address, size) descriptors
##
sub _findMemoryRanges {
  my ($host) = assertNumArgs(1, @_);
  my $iomemContent = assertCommand($host, "sudo cat /proc/iomem")->{stdout};
  my @ramRanges = ();
  my @lines = split(/\n/, $iomemContent);

  foreach my $line (@lines) {
    # Look for top-level range lines (not indented) labeled "System RAM"
    if ($line =~ /^([0-9a-fA-F]+)-([0-9a-fA-F]+) : System RAM$/) {
      my $startHex = $1;
      my $endHex = $2;
      # Using hex() would work if on a 64-bit host, but it generates a
      # warning, and it's cleaner to not require the 64-bit host for
      # running the Perl code, so use BigInt.
      my $start = Math::BigInt->new("0x$startHex");
      my $end = Math::BigInt->new("0x$endHex");

      push(@ramRanges, [$start, $end - $start + 1]);
    }
  }

  return \@ramRanges;
}

#############################################################################
# Given a memory map and desired memory size in bytes, return the
# maximum address.
#
# Written in part by ChatGPT.
#
# @param ranges    An array ref of sorted memory (address,size) descriptors
# @param target    The desired memory size
#
# @return a maximum address to pass to the kernel
#
# @croaks if the requested size is too large
##
sub _findAddressForMemory {
  my ($ranges, $target) = assertNumArgs(2, @_);
  my $accumulated = 0;

  for my $segment (@{$ranges}) {
    my ($start, $size) = @{$segment};
    if (($accumulated + $size) >= $target) {
      my $needed = $target - $accumulated;
      return $start + $needed;
    }
    $accumulated += $size;
  }

  confess("Not enough memory found in the map to satisfy ${target} bytes");
}

#############################################################################
# Set up a host with a specified amount of available memory. Use the
# maximum amount of memory within the acceptable range.
#
# @param host           The host whose memory should be limited
# @param minimumMemory  The minimum amount of memory to limit host to
# @param extraMemory    The amount of memory above the minimum to allow,
#                       if it is available
#
# @croaks on various error conditions
##
sub _limitHostKernelMemory {
  my ($host, $minimumMemory, $extraMemory) = assertNumArgs(3, @_);
  my $hosts = [ $host ];

  # Ensure that each host has at least as much memory as the limit
  my $target = $minimumMemory + $extraMemory;
  my $overhead = 0;
  my $availableMem = getTotalRAM($host);
  my $actualRAM    = getAddressableMemory($host);
  $overhead        = max($overhead, ($actualRAM - $availableMem));
  $target          = min($target, $availableMem);
  assertGENumeric($availableMem, $minimumMemory,
                  "$host reports " . sizeToText($availableMem)
                  . " of memory, should have at least "
                  . sizeToText($minimumMemory));
  $log->info("$host reports " . sizeToText($availableMem)
             . " of available memory, out of " . sizeToText($actualRAM)
             . " actual RAM");
  $log->info(sizeToText($overhead) . " of memory overhead required.");

  my $ramRanges = _findMemoryRanges($host);
  # Reboot the kernel using this much memory
  my $desired         = $target;
  my $iteration       = 0;
  my $maxIterations   = 5;
  while (++$iteration <= $maxIterations) {
    # Figure out how much physical RAM we want to use, then map that
    # to a maximum address based on the machine's memory map.
    my $addrLimit = _findAddressForMemory($ramRanges, int($target + $overhead));
    my $addrLimitKB = int($addrLimit / $KB);
    _rebootWithKernelOption($hosts, "mem", "${addrLimitKB}K");
    my $currMem = getTotalRAM($host);
    # Check that it worked... because of other uses of memory that
    # count against MemTotal, we may not get exactly what we asked,
    # but we should be vaguely in the ballpark.  The caller can try
    # again if it needs to fine-tune the value.
    $log->info("$host now reports " . sizeToText($currMem) . " of memory"
               . " (was shooting for " . sizeToText($desired) . ")");

    if (_isNear($desired, $currMem, "2%")) {
      return;
    }
    $target = $target + ($desired - $currMem) + 3 * $MB;
  }
  # We end the loop here only if we've failed.
  confess("unable to adjust system memory parameter: $host");
}

#############################################################################
# Set up a set of hosts with a specified amount of available memory. Use
# the maximum amount of memory within the acceptable range.
#
# @param hosts          The hosts whose memory should be limited
# @param minimumMemory  The minimum amount of memory to limit hosts to
# @param extraMemory    The amount of memory above the minimum to allow,
#                       if it is available
#
# @croaks on various error conditions
##
sub setupKernelMemoryLimiting {
  my ($hosts, $minimumMemory, $extraMemory) = assertNumArgs(3, @_);

  foreach my $host (@{$hosts}) {
    eval {
      _limitHostKernelMemory($host, $minimumMemory, $extraMemory);
      # Set the appropriate hung task timeout now that we've rebooted.
      setHungTaskTimeout($host);
    };
    if (my $error = $EVAL_ERROR) {
      # Log something here because the remove... cleanup may be verbose.
      $log->error("memory limiting $host failed: $error");
      eval {
        # Remove mem= options even if they weren't working.
        removeKernelMemoryLimiting($hosts);
        foreach my $cleanupHost (@{$hosts}) {
          setHungTaskTimeout($cleanupHost);
        }
      };
      confess("unable to adjust $host system memory parameter: $error");
    }
  }
}

############################################################################
# Remove kernel memory limiting.
#
# @param hosts  The array of hosts to remove memory limits from.
##
sub removeKernelMemoryLimiting {
  my ($hosts) = assertNumArgs(1, @_);
  # We undid the grub configuration change right after it took effect, so
  # we can just reboot to get to an unlimited state.
  rebootMachines(@$hosts);
}

#############################################################################
# Modify the kernel running on a set of machines to the latest rawhide kernel.
#
# @param hosts   The hosts to install the rawhide kernel on.
#
# @croaks on various error conditions
##
sub setupRawhideKernel {
  my ($hosts) = assertNumArgs(1, @_);

  foreach my $host (@$hosts) {
    assertTrue(isFedora($host), "Rawhide only works on Fedora");
  }

  # Install the newfangled kernel.
  foreach my $host (@$hosts) {
    my $gpgKeyPattern
      = "/etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-[1-9]+([0-9])-primary";
    my $rawhideGpgKey
      = assertCommand($host, "bash -O extglob -c '"
			     . "ls -v $gpgKeyPattern | tail -1'")->{stdout};
    chomp($rawhideGpgKey);
    assertCommand($host, "sudo rpm --import $rawhideGpgKey");
    assertCommand($host,
	          "sudo dnf --repo=rawhide -y upgrade kernel kernel-devel");
  }
  rebootMachines(@$hosts);
}

############################################################################
# Restore the kernel running on a set of machines to the usual kernel.
#
# @param hosts  The array of hosts to restore.
#
# @croaks if uninstall fails.
##
sub removeRawhideKernel {
  my ($hosts) = assertNumArgs(1, @_);

  my $leaks;
  foreach my $host (@$hosts) {
    # Find an older kernel that is installed
    my $olderVersion
      = assertCommand($host, "sudo dnf list kernel --installed |"
		      . " grep -v rawhide | tail -n1 |"
		      . " awk '{print \$2}'")->{stdout};
    chomp($olderVersion);
    my $machineId = assertCommand($host, "cat /etc/machine-id")->{stdout};
    chomp($machineId);
    my $arch = assertCommand($host, "uname -m")->{stdout};
    chomp($arch);
    my $olderEntry = "$machineId-$olderVersion.$arch";

    assertCommand($host, "sudo grub2-editenv - set saved_entry=$olderEntry");
  }

  # Reboot the machines to a clean kernel
  rebootMachines(@$hosts);

  foreach my $host (@$hosts) {
    assertCommand($host, "sudo dnf repository-packages rawhide remove -y");
  }
}

############################################################################
# Reboot machines with a kernel option.
#
# @param hosts         Host names of the machines
# @param kernelOption  The option name (i.e. mem)
# @param optionValue   The value to use (i.e. on or 1803839K)
##
sub _rebootWithKernelOption {
  my ($hosts, $kernelOption, $optionValue) = assertNumArgs(3, @_);
  my @grubConfigs = map { Permabit::Grub->new($_) } @$hosts;

  map { $_->modifyOption($kernelOption, $optionValue) } @grubConfigs;
  rebootMachines(@$hosts);
  map { $_->stripOption($kernelOption) } @grubConfigs;
}

1;
