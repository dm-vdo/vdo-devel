##
# Basic vdoStats tests
#
# $Id$
##
package VDOTest::VDOStats;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertEq
  assertEqualNumeric
  assertGTNumeric
  assertNENumeric
  assertMinArgs
  assertNe
  assertNumArgs
  assertRegexpDoesNotMatch
  assertRegexpMatches
);
use Permabit::CommandString::VDOStats;
use Permabit::Constants;
use YAML;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $BLOCK_COUNT = 5000;

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDO device
     deviceType   => "lvmvdo",
     # @ple Use a VDO size of 20GB
     logicalSize  => 20 * $GB,
     # @ple Use an underlying physical size of 10GB
     physicalSize => 10 * $GB,
     # @ple VDO slab bit count
     slabBits     => $SLAB_BITS_TINY,
    );
##

#############################################################################
# Returns an arrayref containing the symmetric difference of the keys in the
# two specified hashes.
#
# @param  firstHash   first hash to check
# @param  secondHash  second hash to check
#
# @return arrayref
##
sub _hashKeysSymmetricDifference {
  my ($self, $firstHash, $secondHash) = assertNumArgs(3, @_);
  my @diff = grep { !defined($secondHash->{$_}) } keys(%{$firstHash});
  push(@diff, grep { !defined($firstHash->{$_}) } keys(%{$secondHash}));
  return \@diff;
}

######################################################################
# Run vdostats with the provided arguments
##
sub _runVDOStats {
  my ($self, %args) = assertMinArgs(2, @_);
  my $machine = $self->getDevice()->getMachine();

  $args{doSudo} = 1;
  $args{binary} = $machine->findNamedExecutable('vdostats');

  my $command = Permabit::CommandString::VDOStats->new($self, \%args);
  $machine->executeCommand("$command");
  return $machine;
}

######################################################################
# Run vdostats on the specified device and assert that it fails
##
sub _assertVDOStatsFailure {
  my ($self, $deviceName, $message) = assertNumArgs(3, @_);
  my $machine = $self->_runVDOStats(deviceName => $deviceName);
  assertNENumeric(0, $machine->getStatus(), $message);
  my $checkString = "'$deviceName': Not a valid running VDO device";
  assertRegexpMatches(qr/$checkString/, $machine->getStderr());
}

######################################################################
# Run vdostats on the specified target and return the YAML output
##
sub _getVDOStatsYAML {
  my ($self, $deviceName) = assertNumArgs(2, @_);
  my $machine = $self->_runVDOStats(deviceName => $deviceName, verbose => 1);
  my $yaml = YAML::Load($machine->getStdout());
  assertDefined($yaml, "stats yaml parsed");
  assertDefined($yaml->{$deviceName}, "yaml identified by specified name");
  return $yaml->{$deviceName};
}

#############################################################################
# Basic VDO testing of human-readable vdoStats output
##
sub testBasic {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  # Write some blocks.
  my $dataset = $self->createSlice(blockCount => $BLOCK_COUNT);
  $dataset->write(tag => "1", fsync => 1);
  my $oneKStats = $device->getVDOStats();

  # Check the non-human stats output (requires manual observation)
  my $stats = $device->getHumanVDOStats({ all => 1 });
  $log->info("Stats reported using --all:\n", $stats);

  # Check the non-SI unit reporting.
  $stats = $device->getHumanVDOStats({ "human-readable" => 1 });
  $log->info("Stats reported using --human-readable:\n", $stats);

  my @lines  = split(/\n/, $stats);
  my ($path, $size, $used, $avail) = split(/\s+/, $lines[1]);

  assertEq($device->{vdoDeviceName}, $path, "volume path");
  assertEq("12.6G", $size, "non-SI size incorrect");
  my $expectedUsed = sprintf("%.1fG",
			     $oneKStats->{"1K-blocks used"} / 1024 / 1024);
  assertEq("$expectedUsed", $used, "non-SI used incorrect");
  my $expectedAvailable
    = sprintf("%.1fG", $oneKStats->{"1K-blocks available"} / 1024 / 1024);
  assertEq("$expectedAvailable", $avail, "non-SI avail incorrect");

  # Check the SI unit reporting.
  $stats = $device->getHumanVDOStats({ si => 1 });
  $log->info("Stats reported using --si:\n", $stats);

  @lines = split(/\n/, $stats);
  ($path, $size, $used, $avail) = split(/\s+/, $lines[1]);

  assertEq($device->{vdoDeviceName}, $path, "volume path");
  assertEq("13.5G", $size, "SI size incorrect");
  $expectedUsed = sprintf("%.1fG",
                          $oneKStats->{"1K-blocks used"} * 1024 / 1000
			  / 1000 / 1000);
  assertEq($expectedUsed, $used, "SI used incorrect");
  $expectedAvailable = sprintf("%.1fG",
                               $oneKStats->{"1K-blocks available"}
                               * 1024 / 1000 / 1000 / 1000);
  assertEq($expectedAvailable, $avail, "SI avail incorrect");

  # Check that df-style output (no volume specified) is identical.
  assertEq($stats, $device->getHumanVDOStats({ si => 1 }, 1),
           "df-style output should be identical with one volume");


  # Check that some stats are disabled when in read-only mode.
  $stats = $device->getVDOStats();
  assertNe("N/A", $stats->{"data blocks used"});
  assertNe("N/A", $stats->{"used percent"});
  assertNe("N/A", $stats->{"saving percent"});
  $device->setReadOnlyMode();
  $stats = $device->getVDOStats();
  assertEq("N/A", $stats->{"data blocks used"});
  assertEq("N/A", $stats->{"used percent"});
  assertEq("N/A", $stats->{"saving percent"});
}

######################################################################
# Test the basic execution of vdostats.
##
sub testBasicExecution {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  my $args = {
              binary => $machine->findNamedExecutable('vdostats'),
              doSudo => 1,
             };

  # Check it works with no target specified.
  my $command = Permabit::CommandString::VDOStats->new($self, $args);
  $machine->assertExecuteCommand("$command");
  my $checkString = $device->getDeviceName();
  assertRegexpMatches(qr/$checkString/, $machine->getStdout());

  # Check it works with full path to target specified.
  $args->{deviceName} = $device->getVDOSymbolicPath();
  $command = Permabit::CommandString::VDOStats->new($self, $args);
  $machine->assertExecuteCommand("$command");
  $checkString = $device->getDeviceName();
  assertRegexpMatches(qr/$checkString/, $machine->getStdout());
}

######################################################################
# Test that biosInProgress goes to 0 once all bios have been acknowledged,
# even when VIOs are still active in the packer.
##
sub testBiosInProgress {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  $device->enableCompression();

  # Write some blocks.
  my $dataset = $self->createSlice(blockCount => 100);
  $dataset->write(tag => "1", compress => .74, direct => 1);
  my $stats = $device->getCurrentVDOStats();
  assertEqualNumeric($stats->{'bios in progress write'}, 0,
                     'current write bios in progress should be 0');
  assertGTNumeric($stats->{'current VDO IO requests in progress'}, 0);
}

######################################################################
# Test the path validation of vdostats.
##
sub testPathValidation {
  my ($self) = assertNumArgs(1, @_);

  # Check it rejects non-existent relative paths.
  $self->_assertVDOStatsFailure(".............foo",
                                "failed with non-existent target");
  $self->_assertVDOStatsFailure("./.............foo",
                                "failed with non-existent target");

  # Check it rejects non-existent full paths.
  $self->_assertVDOStatsFailure("/../../../.............foo",
                                "failed with non-existent target");

  # Check it accepts existent full paths which are not vdo devices and they are
  # rejected by the sampling code.  For this we use /dev/urandom.
  $self->_assertVDOStatsFailure("/dev/urandom",
                                "failed with non-vdo target");

}

######################################################################
# Test handling resolved name and path by vdostats.
##
sub testResolvedNameAndPath {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  # Check that the resolved and non-resolved name give us the same results.
  my $resolvedYaml
    = $self->_getVDOStatsYAML($device->getVDODeviceResolvedName());
  my $yaml = $self->_getVDOStatsYAML($device->getVDODeviceName());

  my $diff = $self->_hashKeysSymmetricDifference($resolvedYaml, $yaml);
  assertEqualNumeric(0, scalar(@{$diff}),
                     "separate stats contain the same entries");

  foreach my $key (keys(%{$yaml})) {
    assertEq($resolvedYaml->{$key}, $yaml->{$key},
             "separate stats contain the same data");
  }

  # Check that the resolved and non-resolved paths give us the same results.
  $resolvedYaml = $self->_getVDOStatsYAML($device->getVDODevicePath());
  $yaml = $self->_getVDOStatsYAML($device->getVDOSymbolicPath());

  $diff = $self->_hashKeysSymmetricDifference($resolvedYaml, $yaml);
  assertEqualNumeric(0, scalar(@{$diff}),
                     "separate stats contain the same entries");

  foreach my $key (keys(%{$yaml})) {
    assertEq($resolvedYaml->{$key}, $yaml->{$key},
             "separate stats contain the same data");
  }

  # Check that vdostats doesn't interpret a path that happens to end with the
  # resolved vdo name as actually referencing the vdo device.
  my $name = $device->getDeviceResolvedName();
  $self->_assertVDOStatsFailure("./........./$name",
                                "failed with non-existent target");
}

1;
