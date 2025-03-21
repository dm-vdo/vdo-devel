##
# Test VDO manager defaults for compression and deduplication.
#
# $Id$
##
package VDOTest::CompressDedupeDefaults;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertFalse assertNumArgs assertTrue);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDO device
     deviceType => "lvmvdo",
    );
##

########################################################################
# Test that compression is on by default (no --compression flag) when
# a volume is created and that you can turn it off.
##
sub propertiesDefaultCompression {
  return ( enableCompression => -1 );
}

sub testDefaultCompression {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  assertTrue($device->isVDOCompressionEnabled(),
             "Default compression should be on");
  $device->disableCompression();
  assertFalse($device->isVDOCompressionEnabled(),
              "disableCompression command should disable compression");

}

########################################################################
# Test that compression is on if you turn it on explicitly when a
# volume is created and that you can turn it off.
##
sub propertiesEnableCompressionOnCreate {
  return ( enableCompression => 1 );
}

sub testEnableCompressionOnCreate {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  assertTrue($device->isVDOCompressionEnabled(),
             "Compression should be on");
  $device->disableCompression();
  assertFalse($device->isVDOCompressionEnabled(),
              "disableCompression command should disable compression");

}

#######################################################################
# Test that compression can be turned off when a volume is created and
# that you can turn it on.
##
sub propertiesDisableCompressionOnCreate {
  return ( enableCompression => 0 );
}

sub testDisableCompressionOnCreate {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  assertFalse($device->isVDOCompressionEnabled(),
              "Compression should be off");
  $device->enableCompression();
  assertTrue($device->isVDOCompressionEnabled(),
             "enableCompression command should enable compression");

}

########################################################################
# Test that deduplication is on by default (no --deduplication flag) when
# a volume is created and that you can turn it off.
##
sub propertiesDefaultDeduplication {
  return ( enableDeduplication => -1 );
}

sub testDefaultDeduplication {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  assertTrue($device->isVDODedupeEnabled(),
             "Default deduplication should be on");
  $device->disableDeduplication();
  assertFalse($device->isVDODedupeEnabled(),
              "disableDeduplication command should disable deduplication");

}

########################################################################
# Test that deduplication is on if you turn it on explicitly when a
# volume is created and that you can turn it off.
##
sub propertiesEnableDeduplicationOnCreate {
  return ( enableDeduplication => 1 );
}

sub testEnableDeduplicationOnCreate {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  assertTrue($device->isVDODedupeEnabled(),
             "Deduplication should be on");
  $device->disableDeduplication();
  assertFalse($device->isVDODedupeEnabled(),
              "disableDeduplication command should disable deduplication");

}

#######################################################################
# Test that deduplication can be turned off when a volume is created and
# that you can turn it on.
##
sub propertiesDisableDeduplicationOnCreate {
  return (
          # suppresses check for indexer ready
          disableAlbireo => 1,
          enableDeduplication => 0,
         );
}

sub testDisableDeduplicationOnCreate {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();

  assertFalse($device->isVDODedupeEnabled(),
             "Deduplication should be off");
  $device->enableDeduplication();
  $device->waitForIndex(statusList => [qw(error active)]);
  assertTrue($device->isVDODedupeEnabled(),
             "enableDeduplication command should enable deduplication");

}

1;
