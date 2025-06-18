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
use Permabit::PlatformUtils qw(isRedHat isAdams);
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
#
# Not for use on RHEL 9, as the boot configuration contains options not
# included in the grub configuration.
##
sub _installGrubConfig {
  my ($self) = assertNumArgs(1, @_);
  my $configName = "/boot/grub2/grub.cfg";
  assertCommand($self->{host}, "sudo grub2-mkconfig -o $configName"
                               . " && sudo cat $configName");
}

############################################################################
# Modify the default boot configuration on a host with a new option on the
# kernel command line. Installs the new configuration. Currently only
# permits modifying with key=value options.
#
# @param kernelOption  The kernel option to add
# @param optionValue   The value to set the option to
##
sub modifyOption {
  my ($self, $kernelOption, $optionValue) = assertNumArgs(3, @_);
  if (isRedHat($self->{host})) {
    assertCommand($self->{host},
                  "sudo grubby --update-kernel=DEFAULT "
                  . "--args=${kernelOption}=${optionValue}");
  } else {
    $self->stripOption($kernelOption);
    my $grubCmdline = "GRUB_CMDLINE_LINUX";
    if (isAdams($self->{host})) {
      $grubCmdline = "GRUB_CMDLINE_LINUX_DEFAULT";
    }
    my $GRUB2_EDIT = ('if(/^' . $grubCmdline . '=/) {'
                      . 's/"\n/ ' . $kernelOption . '=' . $optionValue . '"\n/;'
                      . 's/" /"/;'
                      . '}');
    assertCommand($self->{host}, "sudo perl -i.bak -p -e '$GRUB2_EDIT' "
                  . $self->{_conf});
    assertCommand($self->{host}, "sudo grep $kernelOption= " . $self->{_conf},
                  undef, 1);
    $self->_installGrubConfig();
  }
}

############################################################################
# Remove a kernel option from the appropriate boot configuration on a
# host. Installs the new configuration.
#
# @param kernelOption  The kernel option to strip
##
sub stripOption {
  my ($self, $kernelOption) = assertNumArgs(2, @_);
  if (isRedHat($self->{host})) {
    assertCommand($self->{host},
                  "sudo grubby --update-kernel=DEFAULT "
                  . "--remove-args=${kernelOption}");
  } else {
    assertCommand($self->{host}, "sudo sed -i.UNSTRIPPED"
                                 . " 's/ ${kernelOption}=[^ \"]*//' "
                                 . $self->{_conf});
    $self->_installGrubConfig();
  }
}

1;
