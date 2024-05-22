##
# Test that the various slab bits settings produce VDO devices with acceptable
# slab counts.
#
# $Id$
##
package VDOTest::SlabCount01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertLENumeric assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %TEST_TABLE
  = (
     # This is the smallest configuration anywhere in the VDOTests. For
     # instance, these values are used by FullBase to make a VDO that can
     # quickly be completely filled.
     "testTinyTiny"   => {
                          expectedSlabs => 1,
                          logicalSize   => 2 * $GB,
                          physicalSize  => 375 * $MB,
                          slabBits      => $SLAB_BITS_TINY,
                         },
     # These values are the configuration used by FullBase for multithreaded
     # tests.  It has more slabs and takes longer to fill.
     "testTinyMulti"  => {
                          expectedSlabs => 5,
                          logicalSize   => 4 * $GB,
                          physicalSize  => 0.75 * $GB,
                          slabBits      => $SLAB_BITS_TINY,
                         },
     # This is a typical configuration used with $SLAB_BITS_TINY.
     "testTinySmall"  => {
                          expectedSlabs => 39,
                          physicalSize  => 5 * $GB,
                          slabBits      => $SLAB_BITS_TINY,
                         },
     # This is a configuration used by BlockDevice01.
     "testSmallSmall" => {
                          minimumSlabs  => 2,
                          physicalSize  => 2 * $GB,
                          slabBits      => $SLAB_BITS_SMALL,
                         },
     # This is a normal configuration used in the checkin suite.  Only the lab
     # VFARMs have only 138 slabs.
     "testSmall"      => {
                          minimumSlabs  => 138,
                          slabBits      => $SLAB_BITS_SMALL,
                         },
     # This is a normal configuration used in performance tests.  It should
     # work on a FARM, but there will be only one slab.
     "testLarge"      => {
                          minimumSlabs  => 1,
                          slabBits      => $SLAB_BITS_LARGE,
                         },
    );

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple The type of VDO device to use
     deviceType => "lvmvdo",
    );
##

#############################################################################
# Return the suite with all the tests
##
sub suite {
  my ($package) = assertNumArgs(1, @_);
  my $zones = $package->makeDummyTest()->{physicalThreadCount} || 1;
  my $suite = Test::Unit::TestSuite->empty_new($package);
  while (my ($name, $properties) = each(%TEST_TABLE)) {
    my $minimumSlabs
      = $properties->{expectedSlabs} // $properties->{minimumSlabs};
    if ($zones <= $minimumSlabs) {
      my $test = $package->make_test_from_coderef(\&_verifySlabCount,
                                                  "${package}::${name}");
      while (my ($key, $value) = each(%$properties)) {
        $test->{$key} = $value;
      }
      $suite->add_test($test);
    }
  }
  return $suite;
}

########################################################################
##
sub _verifySlabCount {
  my ($self) = assertNumArgs(1, @_);
  my $stats = $self->getDevice()->getVDOStats();
  if (defined($self->{expectedSlabs})) {
    assertEqualNumeric($self->{expectedSlabs}, $stats->{"slab count"});
  }
  if (defined($self->{minimumSlabs})) {
    assertLENumeric($self->{minimumSlabs}, $stats->{"slab count"});
  }
  $log->info("Slab count is $stats->{'slab count'}");
}

1;
