##
# This is a test of turning compression on and off.
#
# $Id$
##
package VDOTest::VDOCompressFlag;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEq
  assertEqualNumeric
  assertFalse
  assertNumArgs
  assertTrue
);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The number of blocks to write. This needs to be large enough to
     # ensure that many blocks have been forced through the packer before we
     # flush requests by calling getVDOStats() [VDO-3115].
     blockCount => 4000,
     # @ple Use a VDO device
     deviceType => "lvmvdo-linear",
     # @ple The offset to start writing at
     offset     => 0,
    );
##

#############################################################################
# Check that the compression flag is correct in the config file.
#
# @param flag 'enabled' or 'disabled'
##
sub _assertConfigFlag {
  my ($self, $flag) = assertNumArgs(2, @_);
  my $device = $self->getDevice();
  assertEqualNumeric($device->isVDOCompressionEnabled() != 0,
                     $flag eq "enabled");
}

#############################################################################
# Turn on compression and check the output.
##
sub _enableCompression {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $deviceName = $device->getDeviceName();
  $device->enableCompression();
  assertTrue($device->isVDOCompressionEnabled(),
             "compression should have been enabled");
}

#############################################################################
# Turn off compression and check the output.
##
sub _disableCompression {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $deviceName = $device->getDeviceName();
  $device->disableCompression();
  assertFalse($device->isVDOCompressionEnabled(),
              "compression should have been disabled");
}

#############################################################################
# See how statistics values change to figure out whether compression is on.
#
# @param oldStats           statistics before writing the file
# @param newStats           statistics after writing the file
# @param expectCompression  false if compression should be off, true if on
##
sub _assertStatsChange {
  my ($self, $oldStats, $newStats, $expectCompression) = assertNumArgs(4, @_);

  my $oldBlocks = $oldStats->{"compressed blocks written"};
  my $newBlocks = $newStats->{"compressed blocks written"};
  my $oldFragments = $oldStats->{"compressed fragments written"};
  my $newFragments = $newStats->{"compressed fragments written"};

  my $compressed
    = (($oldBlocks == $newBlocks) && ($oldFragments == $newFragments)) ? 0 : 1;
  assertEq($expectCompression, $compressed);
}

#############################################################################
# Write data to the VDO device to see if we get compression.
#
# @param expectCompression  false if compression should be off, true if on
##
sub _writeData {
  my ($self, $expectCompression) = assertNumArgs(2, @_);
  my $dataSlice = $self->createSlice(
                                     blockCount => $self->{blockCount},
                                     offset     => $self->{offset},
                                    );
  my $beforeStats = $self->getDevice()->getVDOStats();
  my $tag = "d$self->{offset}";
  $self->{offset} += $self->{blockCount};
  $dataSlice->write(compress => 0.7, tag => $tag, fsync => 1);
  my $afterStats = $self->getDevice()->getVDOStats();
  $self->_assertStatsChange($beforeStats, $afterStats, $expectCompression);
}

#############################################################################
# Reboot of the test machine.
##
sub _reboot {
  my ($self) = assertNumArgs(1, @_);
  # Restart the machine via clean shutdown and wait for it to reboot
  $self->rebootMachineForDevice($self->getDevice());
}

#############################################################################
# Test turning compression off, with rebooting.
##
sub testOn {
  my ($self) = assertNumArgs(1, @_);
  $self->_enableCompression();
  $self->_writeData(1);
  $self->_assertConfigFlag("enabled");
  $self->_reboot();
  $self->_assertConfigFlag("enabled");
  $self->_writeData(1);
  $self->_enableCompression();
  $self->_disableCompression();
  $self->_writeData(0);
  $self->_assertConfigFlag("disabled");
}

#############################################################################
# Test turning compression on, with rebooting.
##
sub testOff {
  my ($self) = assertNumArgs(1, @_);
  $self->_disableCompression();
  $self->_writeData(0);
  $self->_assertConfigFlag("disabled");
  $self->_reboot();
  $self->_writeData(0);
  $self->_assertConfigFlag("disabled");
  $self->_disableCompression();
  $self->_enableCompression();
  $self->_writeData(1);
  $self->_assertConfigFlag("enabled");
}

1;
