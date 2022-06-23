#############################################################################
# Class representing a virtual device created by a MegaRaid::Adapter object.
#
# $Id$
##
package Permabit::MegaRaid::VirtualDeviceConfig;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Storable qw(dclone);

use Permabit::Assertions qw(
  assertDefinedEntries
  assertMinArgs
  assertNumDefinedArgs
);
use List::MoreUtils qw(none);
use Scalar::Util qw(refaddr);

use overload q("")    => \&_as_string,
             q(eq)    => \&_equals,
             q(ne)    => sub {!_equals(@_)},
             q(==)    => \&_num_equals,
             q(!=)    => sub {!_num_equals(@_)};

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{new}
my %properties = (
  # @ple The device cache policy
  deviceCachePol => undef,
  # @ple The disk cache policy
  diskCachePol   => undef,
  # @ple Array ref of disk objects
  disks          => [],
  # @ple The type of RAID
  raidType       => undef,
  # @ple The read cache policy
  readCachePol   => undef,
  # @ple The raid stripe size
  stripeSize     => undef,
  # @ple The write cache policy
  writeCachePol  => undef,
);
##

# raidType may dictate physical properties of a VirtualDevice
my @REQUIRED = qw(raidType);

#############################################################################
# Creates a C<Permabit::MegaRaid::VirtualDeviceConfig> object
#
# @return a new C<Permabit::MegaRaid::VirtualDeviceConfig> object
##
sub new {
  my $invocant = shift;
  my $class = ref($invocant) || $invocant;
  my $self  = bless { %{ dclone(\%properties) }, @_ },
                    $class;
  assertDefinedEntries($self, \@REQUIRED);
  return $self;
}

#############################################################################
##
sub clone {
  my ($self) = assertNumDefinedArgs(1, @_);
  my $clone = $self->new(%$self);

  # We don't want copies of the PhysicalDisks so we need to copy them
  # by hand.
  $clone->{disks} = [@{$self->{disks}}];
  return $clone;
}

#############################################################################
# Gets the list of disks specified in this config.
#
# @return a list of PhysicalDisks
##
sub getDisks {
  my ($self) = assertNumDefinedArgs(1, @_);
  return @{$self->{disks}};
}

#############################################################################
# Sets the list of disks to be used in this config.
#
# @param  disks      An arrayref of disks
##
sub setDisks {
  my ($self, $disks) = assertNumDefinedArgs(2, @_);
  $self->{disks} = $disks;
}

#############################################################################
# Pushes disks on to the existing list of disks
#
# @param  disks      A list of disks to add to this configuration
##
sub addDisks {
  my ($self, @disks) = assertMinArgs(2, @_);
  push(@{$self->{disks}}, @disks);
}

#############################################################################
# Return true of all the disks provided in the config are unassigned (i.e. free)
#
# @return bool
##
sub isAllDisksFree {
  my ($self) = assertNumDefinedArgs(1, @_);
  return none(sub { $_->isInUse() }, @{$self->{disks}});
}

#############################################################################
# Overload eq to perform a deep equality check to compare two virtual devices
# configs for likeness.
##
sub _equals {
  my ($self, $vdc) = @_;

  # Check if we're dealing with a VirtualDevice or not
  if (! ref($vdc) || ! eval { $vdc->isa(__PACKAGE__)}) {
    return 0;
  }

  # Check field count -- this should always pass
  if (scalar(keys %{$self}) != scalar(keys %{$vdc})) {
    return 0;
  }

  # Check that disk objects are the same instances of
  #  PhysicalDisks.
  if (scalar(@{$self->{disks}}) != scalar(@{$vdc->{disks}})) {
    return 0;
  }
  for (my $i = 0; $i < scalar(@{$self->{disks}}); $i++) {
    if ($self->{disks}->[$i] != $vdc->{disks}->[$i]) {
      return 0;
    }
  }

  # Check that the same fields are defined and equal
  foreach my $key (keys %{$self}) {
    if ($key eq "disks") {
      # We handle this above
      next;
    }
    if (defined($self->{$key}) != defined($vdc->{$key})) {
      return 0;
    } elsif (defined($self->{$key}) && ("$self->{$key}" ne "$vdc->{$key}")) {
      return 0;
    }
  }
  return 1;
}

##########################################################################
# Overload == to check that the objects are the same instance
##
sub _num_equals {
  my ($self, $vdc) = @_;
  return refaddr($self) == refaddr($vdc);
}

##########################################################################
# Overload default stringification
##
sub _as_string {
  my $self = shift;
  return "VirtualDeviceConfig(raid:$self->{raidType} disks["
           . join(',', map {$_->{deviceId}} $self->getDisks()) . "])";
}

1;
