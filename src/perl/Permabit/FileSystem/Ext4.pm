##
# Perl object that represents an Ext4 filesystem
#
# $Id$
##
package Permabit::FileSystem::Ext4;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Assertions qw(assertFalse assertNumArgs);
use Storable qw(dclone);

use base qw(Permabit::FileSystem);

#############################################################################
# @paramList{new}
my %PROPERTIES = (
                  # @ple we are a type ext4 fileSystem
                  fsType       => "ext4",
                  # @ple the basic mkfs.ext4 options, with lazy init disabled
                  #      to prevent background zero-block writes after startup
                  mkfsOptions  => [ "-m 0", "-q", "-O ^uninit_bg",
                                    "-E nodiscard,lazy_itable_init=0", ],
                  # @ple mount using the discard option
                  mountOptions => [ "discard" ],
                 );
##

#############################################################################
# Creates a C<Permabit::FileSystem::Ext4>.
#
# @params{new}
#
# @return a new C<Permabit::FileSystem::Ext4>
##
sub new {
  my $invocant = shift;
  return $invocant->SUPER::new(%{ dclone(\%PROPERTIES) },
                               # Overrides previous values
                               @_,);
}

#############################################################################
# @inherit
##
sub resizefs {
  my ($self) = assertNumArgs(1, @_);
  assertFalse($self->{exported});
  my $devPath = $self->{device}->getSymbolicPath();
  $self->{machine}->runSystemCmd("sudo resize2fs $devPath");
}

1;
