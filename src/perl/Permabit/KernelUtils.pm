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
  setupKmemleak
  removeKmemleak
  setupRawhideKernel
  removeRawhideKernel
);

our $VERSION = 1.0;

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $KMEMLEAK = "/sys/kernel/debug/kmemleak";

my $LEAK_PACKAGE_NAMES
  = "kernel-3.10.0-327.4.5.el7.permabit.kmemleak2.x86_64"
    . " kernel-devel-3.10.0-327.4.5.el7.permabit.kmemleak2.x86_64";

############################################################################
# Helper function to build constant regular expressions for known kernel memory
# leaks.
#
# @param name     The process name.
# @param pid      The pid, if it is fixed.
# @param methods  The methods that must appear in the backtrace, in top to
#                 bottom order.  May be empty.
##
sub _qrknownLeak {
  my ($name, $pid, $methods) = assertNumArgs(3, @_);
  my $knownLeak = join("",
                       '^unreferenced object 0x[0-9a-f]{16}\s.*:\n',
                       '\s{2}comm "', $name, '", pid ', $pid, ', .+\n',
                       '\s{2}hex dump .*\n',
                       '\s{2}backtrace:\n',
                       map { '.+\s' . $_ . '\+' } @$methods);
  return qr/$knownLeak/s;
}

#***************************************************************************
# Known kernel memory leaks.  We cannot attribute these kernel memory leaks
# to VDO.
my %KNOWN_LEAKS;

# Known kernel memory leak number 1. This appears as multiple leaks, and is
# reported like this:
#
# unreferenced object 0xffff8801b2c445a0 (size 96):
#  comm "swapper/0", pid 1, jiffies 4294667993 (age 206.183s)
#  hex dump (first 32 bytes):
#    00 68 50 b5 01 88 ff ff 03 00 00 00 05 00 00 00  .hP.............
#    03 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
#  backtrace:
#    [<ffffffff8162a1de>] kmemleak_alloc+0x4e/0xb0
#    [<ffffffff811c0e6c>] kmem_cache_alloc_trace+0x12c/0x220
#    [<ffffffff8128e205>] selinux_sb_alloc_security+0x25/0x90
#    [<ffffffff812873b6>] security_sb_alloc+0x16/0x20
#    [<ffffffff811e349e>] sget+0xae/0x3d0
#    [<ffffffff811e4155>] mount_single+0x35/0xc0
#    [<ffffffff812928d8>] sel_mount+0x18/0x20
#    [<ffffffff811e4279>] mount_fs+0x39/0x1b0
#    [<ffffffff811ffb2f>] vfs_kern_mount+0x5f/0xf0
#    [<ffffffff811ffbd9>] kern_mount_data+0x19/0x30
#    [<ffffffff81ac9d8a>] init_sel_fs+0x61/0xa2
#    [<ffffffff810020e8>] do_one_initcall+0xb8/0x230
#    [<ffffffff81a911f5>] kernel_init_freeable+0x178/0x217
#    [<ffffffff81626a6e>] kernel_init+0xe/0xf0
#    [<ffffffff81647d98>] ret_from_fork+0x58/0x90
#    [<ffffffffffffffff>] 0xffffffffffffffff
#
# unreferenced object 0xffff8801b1cfa1e0 (size 80):
#  comm "swapper/0", pid 1, jiffies 4294667993 (age 206.185s)
#  hex dump (first 32 bytes):
#    e0 f2 15 b4 01 88 ff ff d8 c2 ce b1 01 88 ff ff  ................
#    78 c3 ce b1 01 88 ff ff 01 00 00 00 03 00 00 00  x...............
#  backtrace:
#    [<ffffffff8162a1de>] kmemleak_alloc+0x4e/0xb0
#    [<ffffffff811c0c68>] kmem_cache_alloc+0x128/0x200
#    [<ffffffff8128c1da>] selinux_inode_alloc_security+0x3a/0xa0
#    [<ffffffff812874de>] security_inode_alloc+0x1e/0x20
#    [<ffffffff811fa65d>] inode_init_always+0xed/0x1e0
#    [<ffffffff811fadd0>] alloc_inode+0x30/0xa0
#    [<ffffffff811fcde1>] new_inode_pseudo+0x11/0x60
#    [<ffffffff811fce49>] new_inode+0x19/0x30
#    [<ffffffff812071c7>] simple_fill_super+0xf7/0x1f0
#    [<ffffffff81292ff4>] sel_fill_super+0x24/0x2d0
#    [<ffffffff811e41b2>] mount_single+0x92/0xc0
#    [<ffffffff812928d8>] sel_mount+0x18/0x20
#    [<ffffffff811e4279>] mount_fs+0x39/0x1b0
#    [<ffffffff811ffb2f>] vfs_kern_mount+0x5f/0xf0
#    [<ffffffff811ffbd9>] kern_mount_data+0x19/0x30
#    [<ffffffff81ac9d8a>] init_sel_fs+0x61/0xa2
#
# unreferenced object 0xffff8801b1cd1780 (size 80):
#   comm "swapper/0", pid 1, jiffies 4294667993 (age 204.041s)
#   hex dump (first 32 bytes):
#     d0 14 16 b4 01 88 ff ff 38 17 cd b1 01 88 ff ff  ........8.......
#     28 18 cd b1 01 88 ff ff 01 00 00 00 03 00 00 00  (...............
#   backtrace:
#     [<ffffffff8162a1de>] kmemleak_alloc+0x4e/0xb0
#     [<ffffffff811c0c68>] kmem_cache_alloc+0x128/0x200
#     [<ffffffff8128c1da>] selinux_inode_alloc_security+0x3a/0xa0
#     [<ffffffff812874de>] security_inode_alloc+0x1e/0x20
#     [<ffffffff811fa65d>] inode_init_always+0xed/0x1e0
#     [<ffffffff811fadd0>] alloc_inode+0x30/0xa0
#     [<ffffffff811fcde1>] new_inode_pseudo+0x11/0x60
#     [<ffffffff811fce49>] new_inode+0x19/0x30
#     [<ffffffff812928f4>] sel_make_inode+0x14/0x50
#     [<ffffffff81292f5a>] sel_make_dir+0x3a/0xb0
#     [<ffffffff81293016>] sel_fill_super+0x46/0x2d0
#     [<ffffffff811e41b2>] mount_single+0x92/0xc0
#     [<ffffffff812928d8>] sel_mount+0x18/0x20
#     [<ffffffff811e4279>] mount_fs+0x39/0x1b0
#     [<ffffffff811ffb2f>] vfs_kern_mount+0x5f/0xf0
#     [<ffffffff811ffbd9>] kern_mount_data+0x19/0x30
$KNOWN_LEAKS{MOUNT} = _qrknownLeak("swapper/0", "1",
                                   [qw(mount_single
                                       sel_mount
                                       mount_fs
                                       vfs_kern_mount
                                       kern_mount_data)]);

# Known kernel memory leak number 2.  This appears as a single leak,
# reported like this:
#
# unreferenced object 0xffff8801b32679e0 (size 32):
#   comm "swapper/0", pid 1, jiffies 4294667998 (age 204.992s)
#   hex dump (first 32 bytes):
#     50 7d 0b b6 01 88 ff ff 50 7d 0b b6 01 88 ff ff  P}......P}......
#     00 e0 79 bf 00 00 00 00 00 00 7d bf 00 00 00 00  ..y.......}.....
#   backtrace:
#     [<ffffffff8162a1de>] kmemleak_alloc+0x4e/0xb0
#     [<ffffffff811c0e6c>] kmem_cache_alloc_trace+0x12c/0x220
#     [<ffffffff81395183>] apei_res_add+0xc3/0x120
#     [<ffffffff8139521a>] apei_get_nvs_callback+0x1a/0x20
#     [<ffffffff8136187f>] acpi_nvs_for_each_region+0x38/0x4f
#     [<ffffffff8139586f>] apei_resources_request+0x8f/0x2c0
#     [<ffffffff81ad5259>] erst_init+0x100/0x300
#     [<ffffffff810020e8>] do_one_initcall+0xb8/0x230
#     [<ffffffff81a911f5>] kernel_init_freeable+0x178/0x217
#     [<ffffffff81626a6e>] kernel_init+0xe/0xf0
#     [<ffffffff81647d98>] ret_from_fork+0x58/0x90
#     [<ffffffffffffffff>] 0xffffffffffffffff
#
# or as:
#
#  unreferenced object 0xffff881fcfdb50a0 (size 32):
#    comm "swapper/0", pid 1, jiffies 4294670798 (age 78.223s)
#     hex dump (first 32 bytes):
#       c0 bd 69 d0 1f 88 ff ff c0 bd 69 d0 1f 88 ff ff  ..i.......i.....
#       18 c0 ea 7d 00 00 00 00 3f c0 ea 7d 00 00 00 00  ...}....?..}....
#     backtrace:
#       [<ffffffff8162a1de>] kmemleak_alloc+0x4e/0xb0
#       [<ffffffff811c0e6c>] kmem_cache_alloc_trace+0x12c/0x220
#       [<ffffffff81395183>] apei_res_add+0xc3/0x120
#       [<ffffffff813952c2>] collect_res_callback+0xa2/0xc0
#       [<ffffffff81394d8b>] apei_exec_for_each_entry+0x7b/0xc0
#       [<ffffffff81394e0a>] apei_exec_collect_resources+0x1a/0x20
#       [<ffffffff81ad5244>] erst_init+0xeb/0x300
#       [<ffffffff810020e8>] do_one_initcall+0xb8/0x230
#       [<ffffffff81a911f5>] kernel_init_freeable+0x178/0x217
#       [<ffffffff81626a6e>] kernel_init+0xe/0xf0
#       [<ffffffff81647d98>] ret_from_fork+0x58/0x90
#       [<ffffffffffffffff>] 0xffffffffffffffff
$KNOWN_LEAKS{ACPI1} = _qrknownLeak("swapper/0", "1",
                                  [qw(apei_res_add
                                      apei_get_nvs_callback
                                      acpi_nvs_for_each_region
                                      apei_resources_request
                                      erst_init)]);

$KNOWN_LEAKS{ACPI2} = _qrknownLeak("swapper/0", "1",
                                  [qw(apei_res_add
                                      collect_res_callback
                                      apei_exec_for_each_entry
                                      apei_exec_collect_resources
                                      erst_init)]);

# Known kernel memory leak number 3.  This appears as a single leak,
# reported like this:
#
# unreferenced object 0xffff8801b55da020 (size 32):
#   comm "modprobe", pid 359, jiffies 4294670525 (age 203.746s)
#   hex dump (first 32 bytes):
#     00 01 10 00 00 00 ad de 00 02 20 00 00 00 ad de  .......... .....
#     80 d2 09 a0 ff ff ff ff 00 00 00 00 00 00 00 00  ................
#   backtrace:
#     [<ffffffff8162a1de>] kmemleak_alloc+0x4e/0xb0
#     [<ffffffff811c0e6c>] kmem_cache_alloc_trace+0x12c/0x220
#     [<ffffffff810ec735>] load_module+0x565/0x1b50
#     [<ffffffff810eded6>] SyS_finit_module+0xa6/0xd0
#     [<ffffffff81647e49>] system_call_fastpath+0x16/0x1b
#     [<ffffffffffffffff>] 0xffffffffffffffff
$KNOWN_LEAKS{MODPROBE} = _qrknownLeak("modprobe", '\d+',
                                      [qw(load_module
                                          SyS_finit_module)]);

# Known kernel memory leak number 4.  This appears as multiple leaks,
# reported like this:
#
# unreferenced object 0xffff8801b8c04cc0 (size 32):
#   comm "kthreadd", pid 2, jiffies 4294667359 (age 5625.299s)
#   hex dump (first 32 bytes):
#     01 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00  ................
#     00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
#   backtrace:
#     [<ffffffff8162a1de>] kmemleak_alloc+0x4e/0xb0
#     [<ffffffff811c3c3c>] __kmalloc_track_caller+0x14c/0x260
#     [<ffffffff81186890>] kmemdup+0x20/0x50
#     [<ffffffff8128c10b>] selinux_cred_prepare+0x1b/0x30
#     [<ffffffff81287d16>] security_prepare_creds+0x16/0x20
#     [<ffffffff810ac7d6>] prepare_creds+0xf6/0x1c0
#     [<ffffffff810acddf>] copy_creds+0x2f/0x150
#     [<ffffffff81078f68>] copy_process.part.25+0x358/0x1610
#     [<ffffffff8107a401>] do_fork+0xe1/0x320
#     [<ffffffff8107a666>] kernel_thread+0x26/0x30
#     [<ffffffff810a65f2>] kthreadd+0x2b2/0x2f0
#     [<ffffffff81647d98>] ret_from_fork+0x58/0x90
#     [<ffffffffffffffff>] 0xffffffffffffffff
$KNOWN_LEAKS{KTHREADD} = _qrknownLeak("kthreadd", "2",
                                      [qw(kmemdup
                                          selinux_cred_prepare
                                          security_prepare_creds
                                          copy_creds
                                          do_fork
                                          kernel_thread
                                          kthreadd)]);

# Known kernel memory leak number 5. This appears as a single leak,
# reported like this:
#
# unreferenced object 0xffff881fcfe451e0 (size 48):                                                                                                                                                            
#   comm "kworker/0:0", pid 4, jiffies 4294670155 (age 161.890s)                                                                                                                                               
#   hex dump (first 32 bytes):                                                                                                                                                                                 
#     00 00 00 00 00 00 00 00 0d 01 2d 00 00 00 00 00  ..........-.....                                                                                                                                        
#     00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................                                                                                                                                        
#   backtrace:                                                                                                                                                                                                 
#     [<ffffffff8162a1de>] kmemleak_alloc+0x4e/0xb0                                                                                                                                                            
#     [<ffffffff811c0c68>] kmem_cache_alloc+0x128/0x200                                                                                                                                                        
#     [<ffffffff81384e97>] acpi_os_acquire_object+0x2b/0x2d                                                                                                                                                    
#     [<ffffffff81384ee3>] acpi_ps_alloc_op+0x37/0x67                                                                                                                                                          
#     [<ffffffff81383919>] acpi_ps_get_next_arg+0x2f9/0x3d0                                                                                                                                                    
#     [<ffffffff81383c8f>] acpi_ps_parse_loop+0x29f/0x580                                                                                                                                                      
#     [<ffffffff81384a00>] acpi_ps_parse_aml+0x98/0x28c                                                                                                                                                        
#     [<ffffffff81385255>] acpi_ps_execute_method+0x1c1/0x26c                                                                                                                                                  
#     [<ffffffff8137f9c5>] acpi_ns_evaluate+0x1c1/0x258                                                                                                                                                        
#     [<ffffffff813736dd>] acpi_ev_asynch_execute_gpe_method+0x149/0x1b0                                                                                                                                       
#     [<ffffffff8135fe5e>] acpi_os_execute_deferred+0x14/0x20                                                                                                                                                  
#     [<ffffffff8109d5fb>] process_one_work+0x17b/0x470                                                                                                                                                        
#     [<ffffffff8109e3cb>] worker_thread+0x11b/0x400                                                                                                                                                           
#     [<ffffffff810a5aef>] kthread+0xcf/0xe0                                                                                                                                                                   
#     [<ffffffff81647d98>] ret_from_fork+0x58/0x90                                                                                                                                                             
#     [<ffffffffffffffff>] 0xffffffffffffffff                            
$KNOWN_LEAKS{KWORKER} = _qrknownLeak("kworker/0:0", "4",
                                     [qw(acpi_os_acquire_object
                                         acpi_ps_alloc_op
                                         acpi_ps_get_next_arg
                                         acpi_ps_parse_loop)]);

# Other known kernel memory leaks. This appears as many leaks,
# in systemd, journald, plymouthd, etc.
$KNOWN_LEAKS{SYSTEMD}   = _qrknownLeak("systemd", "1", [],);
$KNOWN_LEAKS{PLYMOUTHD} = _qrknownLeak("plymouthd", '\d+', []);
$KNOWN_LEAKS{SYSTEMCTL} = _qrknownLeak("systemctl", '\d+', []);

$KNOWN_LEAKS{LYMOUTHD} = _qrknownLeak('\(lymouthd\)', '\d+', []);
$KNOWN_LEAKS{JOURNALD} = _qrknownLeak('\(journald\)', '\d+', []);
$KNOWN_LEAKS{YSTEMCTL} = _qrknownLeak('\(ystemctl\)', '\d+', []);

$KNOWN_LEAKS{"SYSTEMD-CGROUPS"} = _qrknownLeak("systemd-cgroups", '\d+', []);
$KNOWN_LEAKS{"SYSTEMD-JOURNAL"} = _qrknownLeak("systemd-journal", '\d+', []);
$KNOWN_LEAKS{"SYSTEMD-UDEVD"}   = _qrknownLeak("systemd-udevd", '\d+', []);
$KNOWN_LEAKS{"SYSTEMD-FSTAB-G"} = _qrknownLeak("systemd-fstab-g", '\d+', []);

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
# Modify the kernel running on a set of machines to a kmemleak-enabled kernel.
#
# @param hosts   The hosts to install the kmemleak kernel on.
#
# @croaks on various error conditions
##
sub setupKmemleak {
  my ($hosts) = assertNumArgs(1, @_);

  # Install the kernel memory leak checker
  foreach my $host (@$hosts) {
    # Only packaged for RHEL7 at the moment.
    assertEq("RHEL7", getDistroInfo($host), "Kmemleak only works on RHEL7");
    assertFalse(isVirtualMachine($host),
                "Kmemleak only works on real machines not $host");
    assertCommand($host, "sudo yum -y install $LEAK_PACKAGE_NAMES");
  }
  _rebootWithKernelOption($hosts, "kmemleak", "on");

  # Things run slower with the memory leak checking turned on, so become
  # less aggressive about detecting blocked tasks.
  foreach my $host (@$hosts) {
    setHungTaskTimeout($host, 5 * $MINUTE);
    # Clear all existing memory leaks, which are definitely not due to the
    # test which hasn't started yet.
    assertCommand($host, "echo clear | sudo tee $KMEMLEAK");
  }
}

############################################################################
# Restore the kernel running on a set of machines to the usual kernel.
#
# @param hosts  The array of hosts to restore.
#
# @croaks if a kernel memory leak was detected
##
sub removeKmemleak {
  my ($hosts) = assertNumArgs(1, @_);

  my $leaks;
  foreach my $host (@$hosts) {
    # Must always check for memory leaks
    my $hostLeaks = _memoryLeakCheck($host);
    # Set $leaks if the test must fail
    $leaks ||= $hostLeaks;
    # Uninstall the kernel memory leak checker
    assertCommand($host, "sudo rpm --erase $LEAK_PACKAGE_NAMES");
  }

  # Reboot the machines to a clean kernel
  rebootMachines(@$hosts);

  if ($leaks) {
    croak($leaks);
  }
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
# Check for memory leaks
#
# @param $host  Host to check for memory leaks on
#
# @return string to croak with, or undef to not croak
##
sub _memoryLeakCheck {
  my ($host) = assertNumArgs(1, @_);
  my $result = eval {
    # What we want to do is "echo scan >$KMEMLEAK", but the destination file
    # cannot be written without superuser permissions.
    assertCommand($host, "echo scan | sudo tee $KMEMLEAK");
    return assertQuietCommand($host, "sudo cat $KMEMLEAK");
  };
  if (!defined($result)) {
    $log->error("Kernel memory leak software failure:\n$EVAL_ERROR");
    return "Kernel memory leak software failure";
  }
  if (!$result->{stdout}) {
    return undef;
  }
  # Eliminate the known leaks.
  my @leaks = split(/^(?=\w)/m , $result->{stdout});
  my $leakCount = scalar(@leaks);
  while (my ($name, $re) = each(%KNOWN_LEAKS)) {
    @leaks = grep { $_ !~ $re } @leaks;
    my $knownCount = $leakCount - scalar(@leaks);
    if ($knownCount > 0) {
      $log->info("$name $leakCount kernel memory leaks ignored on $host");
    }
    $leakCount = scalar(@leaks);
  }
  # Any other leak will fail the test
  if ($leakCount > 0) {
    $log->error("$leakCount kernel memory leaks found on $host\n"
                . join("", @leaks));
    return "Kernel memory leak found";
  }
  return undef;
}

############################################################################
# Reboot machines with a kernel option.
#
# @param hosts         Host names of the machines
# @param kernelOption  The option name (i.e. kmemleak or mem)
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
