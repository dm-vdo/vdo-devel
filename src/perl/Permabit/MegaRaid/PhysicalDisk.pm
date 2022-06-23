#############################################################################
# Class representing a physical disk attached to a MegaRaid adapter
#
# $Id$
##
package Permabit::MegaRaid::PhysicalDisk;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Scalar::Util qw(refaddr weaken);
use Storable qw(dclone);

use Permabit::Assertions qw(
  assertDefined
  assertDefinedEntries
  assertFalse
  assertNumDefinedArgs
  assertTrue
);
use Permabit::Utils qw(arrayDifference);

use overload q("")    => \&_as_string,
             q(==)    => \&_num_equals,
             q(!=)    => sub {!_num_equals(@_)};

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{new}
my %properties = (
  # @ple The device id
  deviceId    => undef,
  # @ple The id of the disk enclosure for this physical disk
  enclosureId => undef,
  # @ple Permabit::UserMachine a disk lives on
  machine     => undef,
  # @ple The disk slot number
  slotNum     => undef,
  # @ple The virtual device that this disk may be associated with
  _vDev       => undef,
);
##

my @OPTIONAL = qw(_vDev);

#############################################################################
# Creates a C<Permabit::MegaRaid::PhysicalDisk> object
#
# @return a new C<Permabit::MegaRaid::PhysicalDisk> object
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self  = bless { %{ dclone(\%properties) }, @_ },
                    $class;

  my $required = arrayDifference([ keys(%properties) ], \@OPTIONAL);
  assertDefinedEntries($self, $required);

  return $self;
}

#############################################################################
# For physical disks that are part of a VirtualDevice, linux exposes
# access to them through /dev/sd? nodes.
#
# @return the device path
##
sub getDevicePath {
  my ($self) = assertNumDefinedArgs(1, @_);
  assertTrue($self->isInUse(), "disk must be assigned to virt device");
  $self->{_vDev}->getDevicePath();
}

#############################################################################
# Enact PhysicalDisk membership to a VirtualDevice
#
# @param virtualDevice  The VirtualDevice to associate with.
#
# @croaks if trying to join a disk that is already a member of a virtual
#         device
##
sub joinDevice {
  my ($self, $virtualDevice) = assertNumDefinedArgs(2, @_);
  assertFalse($self->isInUse(), "already joined");
  # We don't want to hang onto a strong reference here since this is
  #  our parent node and perl's GC wouldn't collect the VirtualDevice
  #  object due to the circular reference. This could also be solved
  #  by creating a specialized DESTROY method in Adapter objects
  #  that broke the circular reference but that seems more fragile.
  weaken($self->{_vDev} = $virtualDevice);
}

#############################################################################
# Release PhysicalDisk membership from a VirtualDevice
##
sub leaveDevice {
  my ($self) = assertNumDefinedArgs(1, @_);
  $self->{_vDev} = undef;
}

#############################################################################
# Returns true if this device is tied to a virtual device
#
# @return bool
##
sub isInUse {
  my ($self) = assertNumDefinedArgs(1, @_);
  return defined $self->{_vDev};
}

#############################################################################
# Get the SMART media wearout indicator for this PhysicalDisk.
#
# @return the wearout indicator
# @croaks if this PhysicalDisk is not attached to a block device
##
sub getMediaWearoutIndicator {
  my ($self) = assertNumDefinedArgs(1, @_);
  assertTrue($self->isInUse(),
                "This disk has no virtual device which must be set in "
                . "order to access it via smartctl");

  my $wearout;
  my $smartCtlCmd = "sudo smartctl -A -d sat+megaraid,$self->{deviceId} "
                    . "-T permissive " . $self->getDevicePath();
  # this command usually returns a non-zero status (but we still get the
  #  info we need)
  $self->{machine}->sendCommand($smartCtlCmd);
  my $stdout = $self->{machine}->getStdout();

  my @lines = split(/\n/, $stdout);
  foreach my $line (@lines) {
    # 179 is the unique line identifier for Used_Rsvd_Blk_Cnt_Tot lines on the
    # current set of SSDs.
    #NOTE: We are using the Used_Rsvd_Blk_Cnt_Tot value instead of the
    # Wear_Leveling_Count, since this is a more realistic number reflecting the
    # amount of life left on the SSD.  For the Samsung 850 pro SSDs, the
    # Wear_Leveling_Count value reaches at or near 0 before any of the reserved
    # blocks have been used.
    if ($line =~ /^\s*179\s/) {
      my @cols = split(/\s+/, $line);
      # The media wearout indicator appears in the 5th column of output
      my $errorStr = "Malformed Used_Rsvd_Blk_Cnt_Tot line found: $line";
      assertDefined($cols[4], $errorStr);
      if ($cols[4] > 100 || $cols[4] < 0) {
        $log->logcroak($errorStr);
      }
      return $cols[4];
    }
  }
  $log->logcroak("Could not parse wearout indicator from: "
                 . join(", ", @lines));
}

##########################################################################
# Overload == to check that the objects are the same instance
##
sub _num_equals {
  my ($self, $pd) = @_;
  return refaddr($self) == refaddr($pd);
}

##########################################################################
# Overload default stringification
##
sub _as_string {
  my $self = shift;
  return "PhysicalDisk(dev:$self->{deviceId} enc:$self->{enclosureId}"
           . " slot:$self->{slotNum})";
}

1;
