##
# Test VDO rebuild behavior when the device dies unexpectedly.
#
# This test uses the "lossy" device to suddenly stop the storage device from
# doing writes.  It expects the rebuild to succeed, and for a vdoAudit to
# succeed.  There are two reasonable cases to run:
#
#   LossyRebuild02::testNoCache*   - no data cache
#   LossyRebuild02::testMiniCache* - small data cache
#
# $Id$
##
package VDOTest::LossyRebuild02;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);

use base qw(VDOTest::LossyBase);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# Return the suite with all the tests
##
sub suite {
  my ($package) = assertNumArgs(1, @_);
  my $suite = Test::Unit::TestSuite->empty_new($package);

  my $options = $package->makeDummyTest();

  my %lossyTypes;
  $lossyTypes{NoCache} = { cacheBlocks => 0, };
  $lossyTypes{MiniCache} = { cacheBlocks => 5, };
  my %dedupeTypes = (
                     "0"  => 0,
                     "45" => 0.45,
                     "90" => 0.9,
                    );
  my %compressTypes = (
                       "0"  => 0,
                       "60" => 0.6,
                       "90" => 0.9,
                      );
  foreach my $lossyKey (keys(%lossyTypes)) {
    foreach my $dedupeKey (keys(%dedupeTypes)) {
      foreach my $compressKey (keys(%compressTypes)) {
        my $name = "${package}::test${lossyKey}D${dedupeKey}C${compressKey}";
        my $test = $package->make_test_from_coderef(\&_testAudit, $name);
        $test->{lossyOptions} = $lossyTypes{$lossyKey};
        $test->{_compress}    = $compressTypes{$compressKey};
        $test->{_dedupe}      = $dedupeTypes{$dedupeKey};
        $suite->add_test($test, $name);
      }
    }
  }
  return $suite;
}

########################################################################
##
sub _testAudit {
  my ($self) = assertNumArgs(1, @_);

  $self->writeInterruptRecoverAndAudit({tag      => "Audit",
                                        compress => $self->{_compress},
                                        dedupe   => $self->{_dedupe}});
}

1;
