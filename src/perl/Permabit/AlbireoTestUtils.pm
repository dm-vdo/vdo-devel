##
# Basic utilities for Albireo tests.
#
# $Id$
##
package Permabit::AlbireoTestUtils;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Fatal qw(open close);
use Carp qw(croak);

use Permabit::Assertions qw(
  assertDefined
  assertMinMaxArgs
  assertNumArgs
);
use Permabit::SystemUtils qw(assertCommand getNfsTempFile);
use Permabit::Triage::Utils qw(getCodename);
use Permabit::Triage::TestInfo qw(%TEST_INFO);
use Permabit::Utils qw(makeFullPath);

use base qw(Exporter);

our @EXPORT_OK = qw(
  addAlbireoEnvironment
  getAlbGenConfig
  getAlbGenConfigFile
  getIndexDevice
);

# Log4perl Logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# Add environment vars for Albireo to the provided hash.
#
# @param  env     The hashref to the environment.
# @param  udsDir  The path to Albireo libraries
# @param  logfile The value to use for UDS_LOGFILE
#
# @return         The augmented environment
##
sub addAlbireoEnvironment {
  my ($env, $udsDir, $logFile) = assertNumArgs(3, @_);
  $env->{LD_LIBRARY_PATH} = "$udsDir:\${LD_LIBRARY_PATH}";
  $env->{UDS_LOGFILE} = $logFile;
  return $env;
}

#############################################################################
# If a value should be treated as a string in the database and isn't undef,
# wrap it in quotes.
#
# @param value        the value of the element to be stringified
#
# @return             a string or undef
##
sub _quoteString {
  my ($value) = assertNumArgs(1, @_);
  if (defined($value)) {
    return "$value";
  }
  return $value;
}

#############################################################################
# Get the codename string to use for 'branch' for the graphing app post
#
# XXX This is a hack and will be replaced during our triage code overhaul
#
# @param testInfoKey      the TEST_INFO key to use to lookup our project
#                         from which we will derive our 'branch' string
##
sub _getBranchStr {
  my ($testInfoKey) = assertNumArgs(1, @_);
  # XXX Hack -- shouldn't have to access TEST_INFO
  assertDefined($TEST_INFO{$testInfoKey});
  my $project = $TEST_INFO{$testInfoKey}{project};
  my $branchStr = lc(getCodename($project));
  return $branchStr;
}


#############################################################################
# Fetch the underlying device the path resides on, on the given host.
#
# @param host           the host
# @param path           the path
#
# @return                the device name
##
sub getIndexDevice {
  my ($host, $path) = assertNumArgs(2, @_);
  # In a few cases the index directory doesn't exist yet, but we still
  # want disk stats, so walk up the tree until we find something.
  my $cmd = ("p=$path; "
             . q(while test ! -e $p; do p=$\(dirname $p\); done ; )
             . q(dev=$\(df $p | awk '/\/dev\// {print $1}'\) ; )
             . q(readlink -f $dev));
  my $dev = assertCommand($host, $cmd)->{stdout};
  chomp($dev);
  return $dev;
}

########################################################################
# Get an albGenTest config
#
# @param size              Size of the entire stream, include dedup
# @param dedupePercentage  The dedupe percentage
# @param chunkSize         albGenTest blocksize
# @param streamName        Optional basename for stream/substreams
#
# @return the config
##
sub getAlbGenConfig {
  my ($size, $dedupePercentage, $chunkSize, $streamName)
    = assertMinMaxArgs(3, 4, @_);
  $streamName ||= "Data";

  if($dedupePercentage < 0 || $dedupePercentage >= 100) {
    croak("Dedupe percentage out of range, [0,100)");
  }

  if ($dedupePercentage == 0) {
    return "stream $streamName simple { length = $size }\n"
         . "run { $streamName }\n";
  }

  # Keep the mixing size small when using the "fixed_length" chain type so
  # HDD dedupe performance tests run with the "runTime" parameter are able to
  # generate the target dedupe level without much susceptibility to odd
  # substream selection distributions.
  my $mixingSize     = 20;
  my $nonDupePercent = 1 - ($dedupePercentage / 100);

  #Add +1 extra stream for now; we'll remove it later if there is no remainder
  my $numStreams   = int(1 / $nonDupePercent) + 1;

  # We might want to consider using Math::BigInt/Float if we expect to be
  # dealing with large numbers here and are sensative to rounding errors.
  my $baseDataSize    = int(($size * $nonDupePercent) / $chunkSize)
                        * $chunkSize;
  my $remainderSize   = int(($size % $baseDataSize) / $chunkSize) * $chunkSize;
  if ($remainderSize == 0) {
    --$numStreams;
  }

  my $albGenConfig = "stream $streamName simple { length = $baseDataSize }\n";

  my $mixStreamDef = "stream Stream mixed {\n"
                     . "  chaintype = fixed_length\n"
                     . "  mixing = $mixingSize\n"
                     . "  seed = $dedupePercentage\n"
                     . "  numsubstreams = $numStreams\n"
                     . "  substream = $streamName\n";

  for (my $i = 2; $i < $numStreams; $i++) {
    $albGenConfig   .= "stream $streamName.$i alias  "
                     . "{ substream = $streamName length = $baseDataSize }\n";
    $mixStreamDef   .= "  substream = $streamName.$i\n";
  }

  if ($remainderSize != 0) {
    $albGenConfig .= "stream $streamName.$numStreams alias  "
                   . "{ substream = $streamName length = $remainderSize }\n";
    $mixStreamDef .= "  substream = $streamName.$numStreams";
  }

  $albGenConfig   .= "$mixStreamDef\n}\n\n"
                   . "run { Stream }\n";

  return $albGenConfig;
}

#############################################################################
# Builds albGenTest config file located on NFS storage.
#
# @param streamSize        Size of the entire stream, include dedup
# @param dedupePercentage  The dedupe percentage
# @param chunkSize         albGenTest blocksize
# @param streamName        Optional basename for stream/substreams
#
# @return the path to the config file.
#
##
sub getAlbGenConfigFile {
  my ($streamSize, $percentDup, $chunkSize, $streamName)
    = assertMinMaxArgs(3, 4, @_);

  my $albGenTestConfigFile = getNfsTempFile('albgentest.config');
  my $albGenTestConfig = getAlbGenConfig($streamSize, $percentDup, $chunkSize,
                                         $streamName);
  open(my $fh, ">", $albGenTestConfigFile);
  print $fh "$albGenTestConfig";
  close($fh);
  return $albGenTestConfigFile;
}

1;
