##
# Test that the various block devices are usable.
#
# $Id$
##
package VDOTest::BlockDevice01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;
use Permabit::GenDataFiles qw(genDataFiles);
use Permabit::Utils qw(parseBytes);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

# This list of device types is expected to work and is tested nightly.
#
# Please keep it sorted by length then alphebetical.
#
my @deviceTypes = qw(
  corruptor
  crypt
  delay
  dory
  flushnop
  fua
  iscsi
  linear
  loop
  lvmvdo
  raw
  thin
  tracer
  vdo
  corruptor-raw
  crypt-raw
  delay-raw
  dory-linear
  dory-raw
  flushnop-raw
  fua-lvmvdo
  fua-raw
  fua-vdo
  iscsi-raw
  linear-raw
  lvmvdo-iscsi
  lvmvdo-linear
  lvmvdo-loop
  lvmvdo-raw
  thin-raw
  tracer-raw
  vdo-linear
  vdo-loop
  vdo-raw
  vdo-stripfua
  lvmvdo-crypt-raw
  lvmvdo-dory-raw
  lvmvdo-iscsi-linear
  lvmvdo-tracer-raw
  thin-lvmvdo-thin
  thin-vdo-thin
  tracer-corruptor-raw
  vdo-crypt-raw
  vdo-dory-raw
  vdo-tracer-raw
  tracer-lvmvdo-tracer-raw
  tracer-vdo-tracer-raw
);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Amount of data to write
     dataSize      => "256MB",
     # @ple The suite method will override this
     deviceType    => "raw",
     # @ple Size of a loop device
     loopSize      => "7GB",
     # @ple Size of a LogicalDevice
     lvmSize       => "7GB",
     # @ple This setting of the Albireo index size lets us test on 5GB of a
     #      logical or loop device.
     memorySize    => 0.25,
     # @ple This setting of the VDO slab bit count lets us test on 2GB of a
     #      logical or loop device
     slabBits      => $SLAB_BITS_SMALL,
     # @ple This test requires a filesystem
     useFilesystem => 1,
    );
##

#############################################################################
# Return the suite with all the tests
##
sub suite {
  my ($package) = assertNumArgs(1, @_);
  my $suite = Test::Unit::TestSuite->empty_new($package);
  my $options = $package->makeDummyTest();
  foreach my $deviceType (@deviceTypes) {
    # Convert vdo-loop to VDOLoop, dory-raw to DoryRaw, etc.
    my @subtypes = split("-", $deviceType);
    @subtypes = map { m/^(fua|iscsi|vdo)$/ ? uc($_) : ucfirst($_) } @subtypes;
    my $name =  join("", $package, "::test", @subtypes);
    my $test = $package->make_test_from_coderef(\&_writeAndVerify, $name);
    $test->{deviceType} = $deviceType;
    $suite->add_test($test);
  }
  return $suite;
}

########################################################################
# Generate data into the filesystem and verify it.
##
sub _writeAndVerify {
  my ($self) = assertNumArgs(1, @_);
  my $dataSet = genDataFiles(
                             dedupe   => 0.25,
                             fs       => $self->getFileSystem(),
                             numBytes => parseBytes($self->{dataSize}),
                             numFiles => 1024,
                            );
  $dataSet->verify();
}

1;
