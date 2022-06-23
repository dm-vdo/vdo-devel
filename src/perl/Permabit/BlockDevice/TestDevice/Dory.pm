##
# Perl object that represents a block device with a short term memory problem.
#
# $Id$
##
package Permabit::BlockDevice::TestDevice::Dory;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(Permabit::BlockDevice::TestDevice);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple block size
     blockSize   => 4 * $KB,
     # @ple The number of cache blocks
     cacheBlocks => 0,
     # @ple whether to return EIO on error
     returnEIO   => undef,
     # @ple mask that can be used to cause the Dory device to tear writes
     tornMask    => undef,
     # @ple modulus that can be used to cause the Dory device to tear writes
     tornModulus => undef,
    );
##

########################################################################
# @inherit
##
sub makeTableLine {
  my ($self) = assertNumArgs(1, @_);
  return join(' ',
              $self->SUPER::makeTableLine(),
              $self->{blockSize},
              $self->{cacheBlocks});
}

########################################################################
# @inherit
##
sub activate {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::activate();
  $self->addPreDeactivationStep(sub { $self->makeAllWritesSucceed(); });
}

########################################################################
# @inherit
##
sub create {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::create();
  if (defined($self->{returnEIO})) {
    $self->_writeSysfsFile($self->{returnEIO}, "returnEIO");
  }

  # Set up parameters for torn writes
  if (defined($self->{tornMask})) {
    $self->_writeSysfsFile($self->{tornMask}, "torn_mask");
  }
  if (defined($self->{tornModulus})) {
    $self->_writeSysfsFile($self->{tornModulus}, "torn_modulus");
  }
}

########################################################################
# Always switch to faking success on all writes before stopping so that
# infinite-retry-on-error loops terminate. Also does some logging.
##
sub makeAllWritesSucceed {
  my ($self) = assertNumArgs(1, @_);
  $self->_logSysfsFile("statistics");
  $self->_logSysfsFile("state");
  $self->_logSysfsFile("cache");

  $self->_writeSysfsFile(0, "returnEIO");
}

########################################################################
# Stop the device performing any writes.
##
sub stopWriting {
  my ($self) = assertNumArgs(1, @_);
  $self->_writeSysfsFile("1", "stop");
}

########################################################################
# Log the contents of a Dory device sysfs file
#
# @param name  The sysfs filename
##
sub _logSysfsFile {
  my ($self, $name) = assertNumArgs(2, @_);
  my $data = $self->getMachine()->cat($self->makeSysfsPath($name));
  $data ||= "<empty>";
  my @lines = split("\n", $data);
  my $spacer = scalar(@lines) > 1 ? "\n" : " ";
  $log->info($self->getSymbolicPath() . " $name:$spacer$data");
}

########################################################################
# Write a text string to a Dory device sysfs file
#
# @param string  The string to write
# @param name    The sysfs filename
##
sub _writeSysfsFile {
  my ($self, $string, $name) = assertNumArgs(3, @_);
  my $path = $self->makeSysfsPath($name);
  $self->runOnHost("echo $string | sudo dd of=$path");
}

1;
