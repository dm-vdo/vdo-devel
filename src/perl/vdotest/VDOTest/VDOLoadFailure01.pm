##
# Test that failing to load VDO doesn't die horribly.  The failures are due to
# invalid device parameters.
#
# $Id$
##
package VDOTest::VDOLoadFailure01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertEvalErrorMatches assertNumArgs assertTrue);
use Permabit::Constants;
use Permabit::Utils qw(parseBytes);

use base qw(VDOTest);

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # what type of VDO device to set up
     deviceType   => "vdo",
     # physical size of the VDO
     physicalSize => "1GB",
     # the number of bits in the VDO slab
     slabBits     => $SLAB_BITS_TINY,
    );
##

#############################################################################
##
sub _tryIllegal {
  my ($self, $valueHash, $failPattern) = assertNumArgs(3, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  my $oldValues = {};
  for my $k ( keys(%$valueHash) ) {
    my $value = $valueHash->{$k};
    $oldValues->{$k} = $device->{$k};
    $device->{$k} = $value;
    $log->info("Setting $k to $value");
  }

  my $currentCursor = $machine->getKernelJournalCursor();
  eval {
    $device->start();
  };
  assertEvalErrorMatches(qr/\s*Failed while running sudo dmsetup create /);
  assertTrue($machine->searchKernelJournalSince($currentCursor, $failPattern));

  for my $k ( keys(%$oldValues) ) {
    $device->{$k} = $oldValues->{$k};
  }
}

#############################################################################
##
sub testIllegalValues {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  $device->stop();

  $self->_tryIllegal({ logicalThreadCount => 101 },
		     "at most 60 'logical' threads are allowed");
  $self->_tryIllegal({ physicalThreadCount => 101 },
		     "at most 16 'physical' threads are allowed");
  $self->_tryIllegal({ bioAckThreadCount => 101 },
		     "at most 100 'ack' threads are allowed");
  $self->_tryIllegal({ bioThreadCount => 101 },
		     "at most 100 'bio' threads are allowed");
  $self->_tryIllegal({ cpuThreadCount => 101 },
		     "at most 100 'cpu' threads are allowed");
  $self->_tryIllegal({ hashZoneThreadCount => 101 },
		     "at most 100 'hash' threads are allowed");
  $self->_tryIllegal({ logicalThreadCount => (1 << 32) },
		     "unsigned integer needed, found");
  $self->_tryIllegal({ physicalThreadCount => (1 << 32) },
		     "unsigned integer needed, found");
  $self->_tryIllegal({ bioAckThreadCount => (1 << 32) },
		     "unsigned integer needed, found");
  $self->_tryIllegal({ bioThreadCount => (1 << 32) },
		     "unsigned integer needed, found");
  $self->_tryIllegal({ cpuThreadCount => (1 << 32) },
		     "unsigned integer needed, found");
  $self->_tryIllegal({ hashZoneThreadCount => (1 << 32) },
		     "unsigned integer needed, found");

  $self->_tryIllegal({ logicalThreadCount => 1, physicalThreadCount => 16 },
		     " physical zones exceeds slab count ");

  $self->_tryIllegal({ compressionType => "elephant"},
		     "unknown compression type");
  $self->_tryIllegal({ compressionType => "elephant:3"},
		     "unknown compression type");
  $self->_tryIllegal({ compressionType => "lz4extra"},
		     "unknown compression type");
  $self->_tryIllegal({ compressionType => "lz4extra:4"},
		     "unknown compression type");
  $self->_tryIllegal({ compressionType => "lz"},
		     "unknown compression type");
  $self->_tryIllegal({ compressionType => "lz:5"},
		     "unknown compression type");
  $self->_tryIllegal({ compressionType => "lz4:ivory"},
		     "integer needed, found");
  $self->_tryIllegal({ compressionType => "lz4:"},
		     "integer needed, found");
}

#############################################################################
##
sub testCorruptGeometry {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();

  $device->stop();
  # Annihilate the geometry block.
  $device->{storageDevice}->ddWrite((bs    => 4096,
                                     count => 1,
                                     if    => "/dev/zero"));

  my $currentCursor = $machine->getKernelJournalCursor();
  eval {
    $device->start();
  };
  assertEvalErrorMatches(qr/\s*Failed while running sudo dmsetup create /);
  my $pattern = " Could not load geometry block";
  assertTrue($machine->searchKernelJournalSince($currentCursor, $pattern));
}

1;
