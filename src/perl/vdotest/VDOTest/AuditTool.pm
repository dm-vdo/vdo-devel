##
# Test the audit tool for both a clean and faulty VDO.
#
# $Id$
##
package VDOTest::AuditTool;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEqualNumeric
  assertNENumeric
  assertNumArgs
  assertRegexpMatches
);
use Permabit::Constants;
use Permabit::SystemUtils qw(assertCommand);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Number of blocks to write
     blockCount => 16000,
     # @ple Use a VDO device
     deviceType => "lvmvdo",
    );
##

#############################################################################
# Write a selection of mostly-duplicate data.
# Stop the VDO, and audit it expecting a clean (passing) result.
# Corrupt a VDO slab's reference count entry, then audit the VDO again,
# to confirm that the auditor will catch the corrupted entry.
##
sub testAuditTool {
  my ($self) = assertNumArgs(1, @_);

  my $slice = $self->createSlice(blockCount => $self->{blockCount});
  $slice->write(tag => "Audit", dedupe => 0.9, fsync => 1);

  # Get a clean audit
  my $device = $self->getDevice();
  $device->stop();
  my $result = $device->doVDOAudit();
  assertEqualNumeric($result->{returnValue}, 0);
  assertRegexpMatches(qr/All pbn references matched./, $result->{stderr});

  # Corrupt a PBN.
  $result = $device->corruptPBNRef();
  assertEqualNumeric($result->{returnValue}, 0);

  # Get a failed audit
  $result = $device->doVDOAudit();
  assertNENumeric($result->{returnValue}, 0);
  assertRegexpMatches(qr/Reference mismatch for pbn /, $result->{stderr});
}

1;
