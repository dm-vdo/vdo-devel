#############################################################################
# GenericCommand wrapper for MegaCli
#
# $Id$
##
package Permabit::MegaRaid::MegaCli;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertLTNumeric
  assertNumDefinedArgs
);

use base qw(Permabit::GenericCommand);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

# XXX Support setting disk cache policy

############################################################################
# MegaCli output for various settings --> cmdline input equivalent
#
# XXX Add more translations
##
my %ENGLISH_TO_CLI = (
  WriteThrough  => "WT",
  WriteBack     => "WB",
  ReadAheadNone => "NORA",
  Enabled       => "CACHED",  # Enables adapter cache
  Disabled      => "DIRECT",  # Disables adapter cache
);

#############################################################################
# @paramList{new}
my %PROPERTIES = (
   # @ple The adapter id
   adapterId             => undef,
   # @ple Add a virtual device
   addVirtDev            => undef,
   # @ple Executable location
   binary                => "/opt/MegaRAID/MegaCli/MegaCli64",
   # @ple The io cache policy (Enabled | Disabled)
   deviceCachePol        => undef,
   # @ple delete a virtual device
   deleteVirtDev         => undef,
   # @ple The disk objects to cull slotNums from when addVirtDev
   disks                 => [],
   # @ple Always need to run with sudo
   doSudo                => 1,
   # @ple Get the adapter info output
   getAdapterInfo        => undef,
   # @ple Get the physical disk info output
   getPhysicalDiskInfo   => undef,
   # @ple Get the physical disk, virtual device mapping
   getPhysDiskVirtDevMap => undef,
   # @ple Get the virtual device info output
   getVirtDevInfo        => undef,
   # @ple The host to run the command on
   host                  => undef,
   # @ple Executable name
   name                  => "MegaCli",
   # @ple The read cache policy (ReadAheadNone)
   readCachePol          => undef,
   # @ple The raid type
   raidType              => undef,
   # @ple The raid stripe size in KB
   stripeSize            => undef,
   # @ple The virtual device id
   virtDevId             => undef,
   # @ple The write cache policy (WriteThrough | WriteBack)
   writeCachePol         => undef,
);
##

#############################################################################
# @inherit
##
sub getProperties {
  my $self = shift;
  return ( $self->SUPER::getProperties(),
           %PROPERTIES );
}

#############################################################################
# @inherit
##
sub getArguments {
  my ($self) = assertNumDefinedArgs(1, @_);

  if (defined $self->{addVirtDev}) {
    # Help with putting the [enclosure:drive,...] string together
    $self->{_driveEncStr} = $self->_getDriveEnclosureStr();
  }

  # Order matters with MegaCli options
  my @args = ();

  # The 'action' argument always comes first
  $self->_addSimpleOption(\@args, "addVirtDev",            "-CfgLDAdd");
  $self->_addSimpleOption(\@args, "initVirtDev",           "-LDInit -Start");
  $self->_addSimpleOption(\@args, "deleteVirtDev",         "-CfgLDDel");
  $self->_addSimpleOption(\@args, "getAdapterInfo",        "-AdpAllInfo");
  $self->_addSimpleOption(\@args, "getPhysicalDiskInfo",   "-PDList");
  $self->_addSimpleOption(\@args, "getPhysDiskVirtDevMap", "-LDPDInfo");

  # Virt drive (aka logical drive) id comes first, if set
  $self->_addValueOption(\@args,  "virtDevId",             "-L");

  # Virtual device creation -- order matters
  $self->_addValueOption(\@args,  "raidType",              "-R");
  $self->_addValueOption(\@args, "_driveEncStr",           "");
  $self->_addValueOption(\@args, "cachePolicy",            "");
  $self->_addValueOption(\@args, "readPolicy",             "");
  $self->_addValueOption(\@args, "writePolicy",            "");
  $self->_addValueOption(\@args,  "stripeSize",            "-strpsz");

  # Adapter id comes last
  $self->_addValueOption(\@args,  "adapterId",             "-A");

  return @args;
}

#############################################################################
# Overrides SUPER::_addValueOption()
#
# XXX Hack to deal with MegaCli's crazy options syntax.
#  -Option<value> <- note the lack of space or "=" between option and value
#  Hard to imagine any other class needing to do this -- so it should just
#  live here.
#
# @param argsRef      A reference to the list of arguments
# @param property     The property of $self to check
# @param flag         The command-line switch to add
##
sub _addValueOption {
  my ($self, $argsRef, $property, $flag) = assertNumDefinedArgs(4, @_);
  if (defined($self->{$property})) {
    $self->_translateOptionValue($property);
    # No ' ' or = between option and value
    push(@{$argsRef}, $flag . $self->{$property});
  }
}

#############################################################################
# Translate, if necessary, the value of a given property into the format
#  MegaCli expects.
#
# @param property    The property whose value may need munging
##
sub _translateOptionValue {
  my ($self, $property) = assertNumDefinedArgs(2, @_);
  if (defined($ENGLISH_TO_CLI{$self->{$property}})) {
    $self->{$property} = $ENGLISH_TO_CLI{$self->{$property}};
  }
}

#############################################################################
# Create and return a [<PhyDriveSlot>:<DriveEnclosure>, ...] string.
#
# This is necessary in the addVirtDev case
#
# @return the driveEnclosureString
# @croaks if self->disks is empty
##
sub _getDriveEnclosureStr {
  my ($self) = assertNumDefinedArgs(1, @_);
  assertLTNumeric(0, scalar(@{$self->{disks}}));
  my @toJoin = map { "$_->{enclosureId}:$_->{slotNum}" } @{$self->{disks}};
  return ("[" . join(",", @toJoin) . "]");
}

#############################################################################
# @inherit
##
sub as_string {
  my $self = shift;
  return "($self->{name} on $self->{host})";
}

1;
