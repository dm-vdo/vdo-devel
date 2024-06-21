##
# Test that creating a vdo doesn't clobber a file system or another VDO volume
#
# $Id: $
##
package VDOTest::VDOClobber;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(
  assertEvalErrorMatches
  assertNumArgs
  assertRegexpMatches
);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple set up an Unmanaged device
     deviceType => "vdo",
    );
##

#############################################################################
##
sub checkClobberOnlyIfForce {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $storage = $device->{storageDevice};

  # Try running vdoformat without force. Should complain about signatures.
  eval {
    $device->formatVDO({ force => undef });
  };
  assertEvalErrorMatches(qr/existing signature|already containing/);
  # Now create a new VDO using vdoformat. This should work.
  $device->{_formatted} = 0;
  $device->start();
  # Wait till start finishes then stop the device and wipe the geometry block.
  $device->getMachine->runSystemCmd("sudo udevadm settle");
  $device->stop();
  $storage->ddWrite((bs => 4096, count => 1, if => "/dev/zero"));
}

#############################################################################
# Test that creating a vdo won't scribble over various types of existing
# content, unless --force is given to vdoformat.
##
sub testClobber {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $storage = $device->{storageDevice};
  my $storagePath = $storage->getDevicePath();
  my $machine = $device->getMachine();

  $device->stop();
  $self->checkClobberOnlyIfForce();

  my @filesystems = qw(ext3 vfat xfs);
  foreach my $fstype (@filesystems) {
    # mkfs.xfs seems to need root privileges
    $machine->runSystemCmd("sudo mkfs -t $fstype $storagePath");
    $self->checkClobberOnlyIfForce();
  }
}

#############################################################################
# Test that vdoformat won't scribble over an open vdo volume, even if --force
# is given.
##
sub testClobberVDOFormatOpen {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  eval {
    $device->formatVDO({format => undef });
  };
  assertRegexpMatches(qr/The device .* is in use/, $machine->getStderr(),
                      "vdoformat failed to properly check open device");
  eval {
    $device->formatVDO();
  };
  assertRegexpMatches(qr/The device .* is in use/, $machine->getStderr(),
                      "vdoformat failed to properly check open device");
}

#############################################################################
# Test that vdoformat won't scribble over an no open vdo volume, unless
# --force is given.
##
sub testClobberVDOFormatNotOpen {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  $device->stop();
  # Try running vdoformat without force. Should complain about signatures.
  eval {
    $device->formatVDO({ force => undef });
  };
  assertEvalErrorMatches(qr/existing signature|already containing/);
  $device->formatVDO();
}

1;
