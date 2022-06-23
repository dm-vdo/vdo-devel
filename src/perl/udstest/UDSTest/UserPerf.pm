##
# Base class for tests which run user mode linux perf tests.  Runs each test in
# its own testcase.
#
# $Id$
##
package UDSTest::UserPerf;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::SystemUtils qw(assertCommand);
use Permabit::Utils qw(makeFullPath);

use base qw(UDSTest::UserSeparated);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

our %PROPERTIES
  = (
     # @ple what class of machine to run the test on
     clientClass => "VDO-PMI",
     # @ple The test suite.
     unitTests   => "performance",
    );

#############################################################################
# @inherit
##
sub getTestCommand {
  my ($self) = assertNumArgs(1, @_);
  my $testHost = $self->{clientNames}[0];
  my $name = "$self->{unitTestName}.perftest";
  my $path = makeFullPath($self->{workDir}, "tests", $name);
  assertCommand($testHost, "(cd $self->{runDir}; ln $path .)");
  return "./$name";
}

1;
