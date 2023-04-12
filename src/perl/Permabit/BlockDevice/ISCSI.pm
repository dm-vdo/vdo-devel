##
# Perl object that represents an iscsi initiator. When an ISCSI device
# is placed on top of some other BlockDevice, it initially acts as a
# pass-through with the same name as its backing store. The migrate method
# may be used to present the backing store to some other host via ISCSI.
#
# $Id$
##
package Permabit::BlockDevice::ISCSI;

use strict;
use warnings FATAL => qw(all);
use Carp qw(
  cluck
  confess
  croak
);
use English qw(-no_match_vars);
use File::Basename qw(basename);
use IO::File;
use Log::Log4perl;

use Permabit::Assertions qw(
  assertDefined
  assertMinArgs
  assertNumArgs
);

use base qw(Permabit::BlockDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my $INITIATOR_FILE = '/etc/iscsi/initiatorname.iscsi';

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple The target
     target    => undef,
     # @ple The target iqn
     targetIQN => undef,
    );
##

########################################################################
# @inherit
##
sub configure {
  my ($self, $arguments) = assertNumArgs(2, @_);
  $self->SUPER::configure($arguments);
  $self->{deviceName} //= $self->getStorageDevice()->getDeviceName();
}

######################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->isPassThrough()) {
    $self->startTarget();
    $self->login();
    my $lsscsiCmd = "lsscsi 2>/dev/null | grep pbit-target";
    my $lsscsi = $self->runOnHostIgnoreErrors($lsscsiCmd);
    my $device = (split('\s', $lsscsi))[-1];
    if ($device) {
      my $addDevCmd = "which lvmdevices && sudo lvmdevices -y --adddev $device";
      my $delDevCmd = "which lvmdevices && sudo lvmdevices -y --deldev $device";
      $self->runOnHost($addDevCmd);
      $self->addDeactivationStep(sub { $self->runOnHost($delDevCmd); });
    }
  }

  $self->SUPER::activate();
}

######################################################################
# Create the iscsi target.
##
sub startTarget {
  my ($self) = assertNumArgs(1, @_);
  my $target = $self->getStorageDevice();
  $target->runOnHost("sudo systemctl start target");

  my $targetNumber = $self->getDeviceID();
  $self->{target} = "pbit-target-$targetNumber";
  $self->runTargetcli('/backstores/block', 'create', "name=$self->{target}",
                     "dev=" . $target->getDevicePath());
  $self->addDeactivationStep(sub { $self->deleteBackstore() });

  $self->{targetIQN} = join('.',
                            'iqn.2017-07.com.permabit.block-device-pm',
                            $targetNumber,
                            'x8664:sn.feeddeadbeef');
  my $result = $self->runTargetcli('/iscsi', 'create', $self->{targetIQN});
  $self->addDeactivationStep(sub { $self->deleteIQN() });
  $self->runTargetcli("/iscsi/$self->{targetIQN}/tpg1/luns", 'create',
                      "/backstores/block/$self->{target}");
  $log->info("started iscsi target $self->{targetIQN}");
}

######################################################################
# Run a targetcli command
#
# @param arguments  A list of arguments
##
sub runTargetcli {
  my ($self, @arguments) = assertMinArgs(2, @_);
  unshift(@arguments, 'sudo targetcli');
  my $command = join(' ', map { /=/ ? "'$_'" : $_ } @arguments);
  $self->getStorageDevice()->runOnHost($command, 1);
}

######################################################################
# Delete the ISCSI backstore entry for the target.
##
sub deleteBackstore {
  my ($self) = assertNumArgs(1, @_);
  $self->runTargetcli('/backstores/block', 'delete', $self->{target});
  delete $self->{target};
}

######################################################################
# Delete the target IQN.
##
sub deleteIQN {
  my ($self) = assertNumArgs(1, @_);
  $self->runTargetcli('/iscsi', 'delete', $self->{targetIQN});
  delete $self->{targetIQN};
}

######################################################################
# Find the initiator name.
#
# @return the initiator name for the current machine
##
sub getInitiatorName {
  my ($self) = assertNumArgs(1, @_);
  my $result = $self->runOnHost("cat $INITIATOR_FILE", 1);
  foreach my $line (split('\n', $result)) {
    if ($line =~ /(iqn\.\S+)/) {
      return $1;
    }
  }

  die("Failed to find initiator name for $self");
}

######################################################################
# Login to the target.
##
sub login {
  my ($self) = assertNumArgs(1, @_);

  my $initiator = $self->getInitiatorName();
  $self->runTargetcli("/iscsi/$self->{targetIQN}/tpg1/acls", 'create',
                      $initiator);

  my $portal = $self->getStorageHost();
  $self->runOnHost("sudo systemctl start iscsid");
  $self->runOnHost("sudo iscsiadm -m node -o new -T $self->{targetIQN}"
                   . " -p $portal", 1);
  $self->runOnHost("sudo iscsiadm -m node -T $self->{targetIQN} -l", 1);
  $self->addDeactivationStep(sub { $self->logout(); });
  $self->runOnHost("sudo udevadm settle");

  my @result = split("\n",
                     $self->runOnHost("sudo iscsiadm -m session -P 3", 1));
  if ($result[-1] =~ /Attached scsi disk (\S+)/) {
    $self->setDeviceName($1);
  } else {
    die("Failed to find device for new iscsi target");
  }
}

######################################################################
# Logout of the iscsi connection.
##
sub logout {
  my ($self) = assertNumArgs(1, @_);
  my $portal = $self->getStorageHost();
  $self->runOnHost("sudo iscsiadm -m node -T $self->{targetIQN} -u", 1);
  $self->runOnHost("sudo iscsiadm -m node -o delete -p $portal", 1);
}

######################################################################
# @inherit
##
sub migrate {
  my ($self, $newMachine) = assertNumArgs(2, @_);
  my $migrate = sub {
    my $currentHost = $self->getMachineName();
    my $newHost = $newMachine->getName();
    $log->info("Migrating ISCSI device from $currentHost to $newHost");

    if ($newHost ne $self->getStorageHost()) {
      # We can't set the device name until we login from the initiator.
      $self->{machine} = $newMachine;
    } else {
      $self->setDeviceName($self->getStorageDevice()->getDeviceName());
      delete $self->{machine};
    }
  };
  $self->runWhileStopped($migrate);
}

########################################################################
# Check whether this device is in pass-through mode.
#
# @return true if the device is currently a pass-through
##
sub isPassThrough {
  my ($self) = assertNumArgs(1, @_);
  return !defined($self->{machine});
}

########################################################################
# @inherit
##
sub getStorageHost {
  my ($self) = assertNumArgs(1, @_);
  return $self->getStorageDevice()->getMachineName();
}

########################################################################
# @inherit
##
sub getDevicePath {
  my ($self) = assertNumArgs(1, @_);
  if ($self->isPassThrough()) {
    return $self->getStorageDevice()->getDevicePath();
  }

  return $self->SUPER::getDevicePath();
}

########################################################################
# @inherit
##
sub resize {
  my ($self, $newSize) = assertNumArgs(2, @_);
  $self->getStorageDevice()->resize($newSize);

  if (defined($self->{targetIQN})) {
    # Rescan the target to detect any physical size changes
    $self->runOnHost("sudo iscsiadm -m node -T $self->{targetIQN} -R", 1);
  }
}

########################################################################
# @inherit
##
sub getMachine {
  my ($self) = assertNumArgs(1, @_);
  return $self->{machine} // $self->SUPER::getMachine();
}

########################################################################
# @inherit
##
sub supportsPerformanceMeasurement {
  # If we go over the network, performance will not be reliable.
  return 0;
}

1;
