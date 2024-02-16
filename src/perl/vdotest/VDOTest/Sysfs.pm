##
# Test the sysfs information associated with the VDO device
#
# $Id$
##
package VDOTest::Sysfs;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use File::Basename;
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEq
  assertMinMaxArgs
  assertNumArgs
  assertRegexpMatches
);
use Permabit::Constants;
use Permabit::Utils qw(makeFullPath);
use Permabit::Version qw($VDO_VERSION);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple Use a VDO device
  deviceType => "lvmvdo",
);
##

#############################################################################
##
sub testSysfs {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $majorMinor = join(":", $device->getVDODeviceMajorMinor());

  # This is the directory in /sys where kvdo is found
  my $sysModDir = makeFullPath("/sys/module", $device->getModuleName());

  # Check version in new location
  $self->_readonlyCheck(makeFullPath($sysModDir, "version"), $VDO_VERSION);

  # This is the directory in /sys where the kvdo parameters are found
  my $sysModParmDir = makeFullPath($sysModDir, "parameters");

  # Check parameters writable only by root
  $self->_writeCheck(makeFullPath($sysModParmDir,
                                  "deduplication_timeout_interval"),
                     5000, 4000);

  $self->_writeCheck(makeFullPath($sysModParmDir, "log_level"),
		     "6", "7");
  $self->_writeCheckIfExists(makeFullPath($sysModParmDir,
					  "max_discard_sectors"),
                             8, 64);
  $self->_writeCheck(makeFullPath($sysModParmDir, "max_requests_active"),
                     $DEFAULT_MAX_REQUESTS_ACTIVE, 1000);
  $self->_writeCheck(makeFullPath($sysModParmDir,
                                  "min_deduplication_timer_interval"),
                     100, 200);

  # This is the directory in /sys where block device parameters are found
  my $blockDevDir = makeFullPath("/sys/dev/block", $majorMinor);

  # This is the directory in /sys where vdo device parameters are found
  my $sysModDevDir = makeFullPath($blockDevDir, "vdo");

  # Check readonly parameters
  $self->_readonlyCheck(makeFullPath($sysModDevDir, "compressing"), 0);
  $self->_readonlyCheck(makeFullPath($sysModDevDir, "discards_active"), 0);
  $self->_readonlyCheck(makeFullPath($sysModDevDir, "discards_maximum"), 0);
  $self->_readonlyCheck(makeFullPath($sysModDevDir, "instance"), "1");
  $self->_readonlyCheck(makeFullPath($sysModDevDir, "requests_active"), 0);
  $self->_readonlyCheck(makeFullPath($sysModDevDir, "requests_limit"),
                        $DEFAULT_MAX_REQUESTS_ACTIVE);
  $self->_readonlyCheck(makeFullPath($sysModDevDir, "requests_maximum"),
                        undef);
  $self->_readonlyCheck(makeFullPath($sysModDevDir, "dedupe/status"),
                        "online");

  # Check parameters writable only by root
  $self->_writeCheck(makeFullPath($sysModDevDir, "discards_limit"),
                     $DEFAULT_MAX_REQUESTS_ACTIVE * $DEFAULT_DISCARD_RATIO,
                     1234);

  # Check reading of block device parameters
  $self->_readCheck(makeFullPath($blockDevDir, "alignment_offset"), 0);
  $self->_readCheck(makeFullPath($blockDevDir, "discard_alignment"), 0);
  $self->_readCheck(makeFullPath($blockDevDir, "ro"), 0);
  $self->_readCheck(makeFullPath($blockDevDir, "dm/suspended"), 0);
  $self->_readCheck(makeFullPath($blockDevDir, "queue/discard_granularity"),
                    4 * $KB);
  $self->_readCheck(makeFullPath($blockDevDir, "queue/discard_max_bytes"),
                    4 * $KB);
  $self->_readCheck(makeFullPath($blockDevDir, "queue/hw_sector_size"),
                    4 * $KB);
  $self->_readCheck(makeFullPath($blockDevDir, "queue/logical_block_size"),
                    4 * $KB);
  $self->_readCheck(makeFullPath($blockDevDir, "queue/minimum_io_size"),
                    4 * $KB);
  $self->_readCheck(makeFullPath($blockDevDir, "queue/optimal_io_size"),
                    4 * $KB);
  $self->_readCheck(makeFullPath($blockDevDir, "queue/physical_block_size"),
                    4 * $KB);
}

#############################################################################
##
sub testSysfsStats {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $majorMinor = join(":", $device->getVDODeviceMajorMinor());

  # This is the directory in /sys where the kvdo statistics are found
  my $statsDir = makeFullPath("/sys/dev/block", $majorMinor, "vdo/statistics");

  my $stats = $device->getVDOStats();
  # Statistics called out in VDOSTORY-72 as supported/documented
  $self->_readCheck(makeFullPath($statsDir, "data_blocks_used"), undef);
  $self->_readCheck(makeFullPath($statsDir, "logical_blocks_used"), undef);
  $self->_readCheck(makeFullPath($statsDir, "physical_blocks"),
		   $stats->{"physical blocks"});
  $self->_readCheck(makeFullPath($statsDir, "logical_blocks"),
		   $stats->{"logical blocks"});
  $self->_readCheck(makeFullPath($statsDir, "mode"),
		   $stats->{"mode"});

  # Other statistics
  $self->_readCheck(makeFullPath($statsDir, "block_size"),
		   $stats->{"block size"});
  $self->_readCheck(makeFullPath($statsDir, "bios_out_read"), undef);
}

#############################################################################
# Test the max length checking works properly.
##
sub testSysfsLength {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $majorMinor = join(":", $device->getVDODeviceMajorMinor());

  # This is the directory in /sys where the kvdo parameters are found
  my $sysModDir = makeFullPath("/sys/module", $device->getModuleName(),
			       "parameters");
  $self->_write(makeFullPath($sysModDir, "max_requests_active"),
		"2147483647", 1, 1);
  $self->_write(makeFullPath($sysModDir, "max_requests_active"),
		"2147483648", 1, 0, "Numerical result out of range");
  $self->_write(makeFullPath($sysModDir, "min_deduplication_timer_interval"),
		"4294967295", 1, 1);
  $self->_write(makeFullPath($sysModDir, "min_deduplication_timer_interval"),
		"4294967296", 1, 0, "Numerical result out of range");
  $self->_write(makeFullPath($sysModDir, "deduplication_timeout_interval"),
		"4294967295", 1, 1);
  $self->_write(makeFullPath($sysModDir, "deduplication_timeout_interval"),
		"4294967296", 1, 0, "Numerical result out of range");
}

#############################################################################
# Check that a sysfs file is readable
#
# @param path      The pathname
# @param expected  Value we expect to read, or undef if any value is acceptable
##
sub _readCheck {
  my ($self, $path, $expected) = assertNumArgs(3, @_);
  my $machine = $self->getDevice()->getMachine();
  $machine->runSystemCmd("cat $path");
  my $value = $machine->getStdout();
  chomp($value);
  if (defined($expected)) {
    assertEq($expected, $value, "Value from $path");
  } else {
    $log->info("$path: $value");
  }
}

#############################################################################
# Check that a sysfs file is readonly
#
# @param path      The pathname
# @param expected  Value we expect to read, or undef if any value is acceptable
##
sub _readonlyCheck {
  my ($self, $path, $expected) = assertNumArgs(3, @_);
  $self->_readCheck($path, $expected);
  $self->_write($path, $expected // 0, 0, 0);
  $self->_write($path, $expected // 0, 1, 0);
}

#############################################################################
# Write a value to a sysfs file
#
# @param  path     The pathname
# @param  value    Value to write
# @param  sudo     true to write as root, false to write as peon
# @param  succeed  true to expect success, false to expect failure
# @oparam error    error msg to look for
##
sub _write {
  my ($self, $path, $value, $sudo, $succeed, $error)
    = assertMinMaxArgs(["Permission denied"], 5, 6, @_);
  my $command = $sudo ? "echo $value | sudo tee $path" : "echo $value >$path";
  my $machine = $self->getDevice()->getMachine();
  if ($succeed) {
    $machine->runSystemCmd($command);
  } else {
    my $errno = $machine->sendCommand($command);
    assertEq(1, $errno, "Writing to $path");
    assertRegexpMatches(qr/$error/, $machine->getStderr());
  }
}

#############################################################################
# Check that a sysfs file is writable
#
# @param path      The pathname
# @param expected  Value we expect to read
# @param trial     Value to write (and succeed as root, fail as peon)
##
sub _writeCheck {
  my ($self, $path, $expected, $trial) = assertNumArgs(4, @_);
  $self->_readCheck($path, $expected);
  $self->_write($path, $trial, 0, 0);
  $self->_write($path, $trial, 1, 1);
  $self->_readCheck($path, $trial);
  $self->_write($path, $expected, 1, 1);
  $self->_readCheck($path, $expected);
}

#############################################################################
# If a sysfs file exists, check that it is writable
#
# @param path      The pathname
# @param expected  Value we expect to read
# @param trial     Value to write (and succeed as root, fail as peon)
##
sub _writeCheckIfExists {
  my ($self, $path, $expected, $trial) = assertNumArgs(4, @_);
  my $machine = $self->getDevice()->getMachine();
  if ($machine->pathExists($path)) {
    $self->_writeCheck($path, $expected, $trial);
  }
}

1;
