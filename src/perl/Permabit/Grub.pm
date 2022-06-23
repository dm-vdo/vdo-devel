##
# Utilities for editing grub configurations of hosts. Assumes grub version 2.
#
# $Id$
##
package Permabit::Grub;

use strict;
use warnings FATAL => qw(all);
use Carp qw(confess);
use English qw(-no_match_vars);
use List::Util qw(max);
use Log::Log4perl;
use Permabit::Assertions qw(assertDefined assertNumArgs);
use Permabit::Constants;
use Permabit::SystemUtils qw(
  assertCommand
  runCommand
);
use Storable qw(dclone);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{newProperties}
my %PROPERTIES =
  (
   #########################
   # constructor arguments
   #########################

   # @ple the host to edit
   host     => undef,

   #########################
   # member variables
   #########################

   # the grub configuration file
   _conf    => "/etc/default/grub"

  );
##

######################################################################
# Instantiate a Permabit::Grub.
#
# @param host   The host in question
##
sub new {
  my ($invocant, $host) = assertNumArgs(2, @_);
  my $class = ref($invocant) || $invocant;
  my $self = bless { %{ dclone(\%PROPERTIES) }, host => $host }, $invocant;
  assertDefined($self->{host});
  return $self;
}

############################################################################
# Install the current grub configuration, if necessary.
##
sub installConfig {
  my ($self) = assertNumArgs(1, @_);

  # If there exists a /boot/efi/EFI/redhat/grub.cfg, we assume that the
  # rest of the grub config points at it. Despite the name, it's still
  # BIOS-compatible; it is just there so that the same file structure
  # exists on both EFI and BIOS machines.
  my $configName = "/boot/efi/EFI/redhat/grub.cfg";
  my $result = runCommand($self->{host}, "sudo test -e $configName");
  if ($result->{returnValue} != 0) {
    $configName = "/boot/grub2/grub.cfg";
  }
  assertCommand($self->{host}, "sudo grub2-mkconfig -o $configName"
                               . " && sudo cat $configName");
}

############################################################################
# Modify the appropriate grub configuration file on a host with a new option
# on the kernel command lines. Does not necessarily install the new
# configuration. Currently only permits modifying with key=value options.
#
# @param kernelOption  The kernel option to add
# @param optionValue   The value to set the option to
##
sub modifyOption {
  my ($self, $kernelOption, $optionValue) = assertNumArgs(3, @_);
  $self->stripOption($kernelOption);
  my $GRUB2_EDIT = ('if(/^GRUB_CMDLINE_LINUX=/) {'
                    . 's/"\n/ ' . $kernelOption . '=' . $optionValue . '"\n/;'
                    . 's/" /"/;'
                    . '}');
  assertCommand($self->{host}, "sudo perl -i.bak -p -e '$GRUB2_EDIT' "
                               . $self->{_conf});
  assertCommand($self->{host}, "sudo grep $kernelOption= " . $self->{_conf},
                undef, 1);
}

############################################################################
# Remove a kernel option from the appropriate grub configuration file on a
# host. Does not necessarily install the new configuration.
#
# @param kernelOption  The kernel option to strip
##
sub stripOption {
  my ($self, $kernelOption) = assertNumArgs(2, @_);
  assertCommand($self->{host}, "sudo sed -i.UNSTRIPPED"
                               . " 's/ ${kernelOption}=[^ \"]*//' "
                               . $self->{_conf});
}

1;
