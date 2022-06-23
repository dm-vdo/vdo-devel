##
# Perl object that represents an Nfs mounted filesystem
#
# $Id$
##
package Permabit::FileSystem::Nfs;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Assertions qw(assertDefined assertNumArgs assertType);
use Storable qw(dclone);

use base qw(Permabit::FileSystem);

# TODO: Make fsck check the remote filesystem on its native machine

#############################################################################
# @paramList{new}
my %PROPERTIES = (
                  # @ple The filesystem we are mounting
                  fs     => undef,
                  # @ple we are a type nfs filesystem
                  fsType => "nfs",
                  # @ple Mount with options matching standards
                  mountOptions => ["timeo=600", "retrans=2"],
                 );
##

#############################################################################
# Creates a C<Permabit::FileSystem::Nfs>.
#
# @params{new}
#
# @return a new C<Permabit::FileSystem::Nfs>
##
sub new {
  my $invocant = shift;
  my $self = $invocant->SUPER::new(%{ dclone(\%PROPERTIES) },
                                   # Overrides previous values
                                   @_,);
  assertDefined($self->{fs});
  assertType("Permabit::FileSystem", $self->{fs});
  return $self;
}

#############################################################################
# @inherit
##
sub getMountSpecial {
  my ($self) = assertNumArgs(1, @_);
  my $fs = $self->{fs};
  return $fs->getMachine()->getName() . ":" . $fs->getMountDir();
}

#############################################################################
# @inherit
##
sub isLocal {
  my ($self) = assertNumArgs(1, @_);
  return 0;
}

#############################################################################
# @inherit
##
sub usingDiscard {
  my ($self) = assertNumArgs(1, @_);
  # We must delegate this to the underlying filesystem
  return $self->{fs}->usingDiscard();
}

1;
