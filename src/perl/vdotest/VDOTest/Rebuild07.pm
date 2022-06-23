##
# Test VDO rebuild and the integrity of the filesystem in 512e mode.
#
# This is basically the Rebuild03 test run using and exercising the 512 byte
# block size.
#
# $Id$
##
package VDOTest::Rebuild07;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Constants;

use base qw(VDOTest::Rebuild03);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple All the files are smaller than 4KB in this test
     compressibleChunkSize => 4 * $KB,
     # @ple Make each chunk mostly compressible
     compressibility       => 55,
     # @ple Emulate a 512 byte block device
     emulate512Enabled     => 1,
     # @ple Enable compression by VDO
     enableCompression     => 1,
    );
##

#############################################################################
# @inherit
##
sub getDataSetProps03 {
  my ($self, $subdir) = assertNumArgs(2, @_);
  my $props = {
               "gen.root.dir"                => $subdir,
               "gen.large.num"               => 50000,
               "gen.large.min"               => 400,
               "gen.large.max"               => 4 * $KB,
               "gen.large.numCoalescent"     => 12500,
               "gen.compressible.percentage" => $self->{compressibility},
               "gen.compressible.blockSize"  => $self->{compressibleChunkSize},
              };
  return $props;
}

1;
