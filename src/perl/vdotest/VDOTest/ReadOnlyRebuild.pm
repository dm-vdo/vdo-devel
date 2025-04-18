##
# Check that we don't lose everything in read-only mode.
#
# This test must be run with a VDO device at the top level.
#
# $Id$
##
package VDOTest::ReadOnlyRebuild;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertEvalErrorMatches assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest::RebuildBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks in each dataset
     blockCount    => 10000,
     # @ple Use a small logical size for fast full rebuild
     logicalSize   => 5 * $GB,
     # @ple A hidden list full of dataset locations
     _dataSets     => undef,
     # @ple This test cannot use a filesystem.
     useFilesystem => 0,
    );
##

#############################################################################
# Write some blocks, sync, go into read-only mode. Check that all the data are
# still readable. Come out of read-only mode, and make sure we can read,
# write, and recover.
##
sub _doForceRebuild {
  my ($self) = assertNumArgs(1, @_);
  my $device  = $self->getDevice();
  my $dataSize = $self->{blockCount} * $self->{blockSize};
  $self->{_dataSets}
     = [
        $self->createSlice(
                           blockCount => $self->{blockCount},
                          ),
        $self->createSlice(
                           blockCount => $self->{blockCount} / 2,
                           offset     => $self->{blockCount},
                          ),
       ];

  $self->{_dataSets}[0]->write(tag => "data0", fsync => 1);
  # Shut down and, while shut down, go into read-only mode.
  $device->setReadOnlyMode();

  # Verify that the pre-sync data are all still there.
  $self->{_dataSets}[0]->verify();

  # Verify that we cannot write data.
  eval {
    $device->ddWrite(
                     if    => "/dev/urandom",
                     count => 1,
                     bs    => $self->{blockSize},
                     oflag => "direct",
                    );
  };
  assertEvalErrorMatches(qr{Input/output error}, "VDO is read-only");

  # Verify that the vdo is active.
  my $command = "sudo dmsetup status " . $device->getVDODeviceName();
  $device->getMachine()->assertExecuteCommand($command);

  # Take the VDO out of read-only mode.
  $device->stop();
  $device->forceRebuild();
}

#############################################################################
# Verify that we can do a normal recovery with new data after a full rebuild.
##
sub testRebuildAfterFullRebuildAndOverwrite {
  my ($self) = assertNumArgs(1, @_);
  $self->_doForceRebuild();
  $self->{_dataSets}[0]->verify();

  $self->{_dataSets}[1]->write(tag => "data1", fsync => 1);
  $self->recoverAndRestartVDO();
  $self->{_dataSets}[0]->verify();
  $self->{_dataSets}[1]->verify();
}

#############################################################################
# Verify that we can do a full rebuild with new data after a full rebuild.
##
sub testThreeFullRebuilds {
  my ($self) = assertNumArgs(1, @_);
  $self->_doForceRebuild();
  $self->{_dataSets}[0]->verify();

  my $device = $self->getDevice();
  $device->stop();
  $device->setReadOnlyMode();
  $device->forceRebuild();
  $self->{_dataSets}[0]->verify();
  $self->{_dataSets}[1]->write(tag => "data1", fsync => 1);

  $device->stop();
  $device->setReadOnlyMode();
  $device->forceRebuild();
  $self->{_dataSets}[0]->verify();
  $self->{_dataSets}[1]->verify();
}

1;
