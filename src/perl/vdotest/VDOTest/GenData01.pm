##
# Use genDataSet to generate and verify data
#
# $Id$
##
package VDOTest::GenData01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertEqualNumeric assertNumArgs);
use Permabit::Constants;
use Permabit::GenDataFiles qw(genDataFiles);
use Permabit::LabUtils qw(isVirtualMachine);
use Permabit::Utils qw(parseBytes);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

########################################################################
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Amount of data to write in each of the four datasets
     dataSize      => 800 * $MB,
     # @ple Use a VDO device
     deviceType    => "lvmvdo",
     # @ple This test requires a filesystem
     useFilesystem => 1,
    );
##

########################################################################
# Create a filesystem, generate data into it serially, and then
# verify it.
##
sub testSerialData {
  my ($self)      = assertNumArgs(1, @_);
  my $beforeStats = $self->getVDOStats();
  my @datasetList;
  foreach my $numFiles (1, 32, 1024, 32768) {
    push(@datasetList, genDataFiles(
                                    dedupe   => 0.25,
                                    fs       => $self->getFileSystem(),
                                    numBytes => parseBytes($self->{dataSize}),
                                    numFiles => $numFiles,
                                   ));
  }

  map { $_->verify() } @datasetList;
  my $afterStats = $self->getVDOStats();
  if (defined($beforeStats)
      && defined($afterStats)
      && !isVirtualMachine($self->getDevice()->getMachine()->getName())
      && !$self->{lowMemoryTest}) {
    assertEqualNumeric($beforeStats->{"dedupe advice timeouts"},
                       $afterStats->{"dedupe advice timeouts"});
  }
}

1;
