##
# Perl object that represents the debugfs filesystem
#
# $Id$
##
package Permabit::FileSystem::Debug;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Permabit::Assertions qw(assertEq
                            assertMinMaxArgs
                            assertNumArgs
                            assertTrue);
use Storable qw(dclone);

use base qw(Permabit::FileSystem);

my $MOUNT_DIR = "/sys/kernel/debug";

#############################################################################
# @paramList{new}
my %PROPERTIES = (
                  # @ple we are a type debugfs fileSystem
                  fsType       => "debugfs",
                  # @ple pathname of the mount directory
                  mountDir     => $MOUNT_DIR,
                 );
##

#############################################################################
# Creates a C<Permabit::FileSystem::Debug>.
#
# @params{new}
#
# @return a new C<Permabit::FileSystem::Debug>
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
sub fsck {
  my ($self) = assertNumArgs(1, @_);
}

#############################################################################
# @inherit
##
sub getFreeBlocks {
  my ($self, $blockSize) = assertNumArgs(2, @_);
  return 0;
}

#############################################################################
# @inherit
##
sub getMountSpecial {
  my ($self) = assertNumArgs(1, @_);
  return "nodev";
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
sub makeSubdir {
  my ($self, $name) = assertNumArgs(2, @_);
  assertTrue(0, "cannot make subdir of debug filesystem");
}

#############################################################################
# @inherit
##
sub mount {
  my ($self, $mountDir) = assertMinMaxArgs([undef], 1, 2, @_);
  $mountDir ||= $self->{mountDir};

  # The mount directory is only valid if it is our specific mount directory.
  assertEq($MOUNT_DIR, $mountDir, "invalid mount directory specified");

  # Only do the mount if it's not already mounted.
  my $status = $self->{machine}->sendCommand("mount | grep $mountDir");
  assertTrue(($status == 0) || ($status == 1), "expected grep status");

  if ($status != 0) {
    $self->SUPER::mount($mountDir);
  }
}

#############################################################################
# @inherit
##
sub stop {
  my ($self) = assertNumArgs(1, @_);
}

#############################################################################
# @inherit
#
# unmount is a no-op for one reason: the superclass implementation performs
# a removal of the mount directory and the mount directory for debugfs is not
# ours to manipulate.  Leaving debugfs mounted causes no problem.
##
sub unmount {
  my ($self) = assertNumArgs(1, @_);
}

1;
