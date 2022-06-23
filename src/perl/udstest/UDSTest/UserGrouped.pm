##
# Base class for tests which run user mode linux unit tests.  Runs all the
# tests quickly grouped into a single testcase.
#
# $Id$
##
package UDSTest::UserGrouped;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::SystemUtils qw(assertCommand runCommand);
use Permabit::Utils qw(makeFullPath);

use base qw(UDSTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my @USER_TEST_FILES = (
                       {
                        files => [
                                  "build/*.a",
                                  "build/*.so",
                                  "build/*.so.*",
                                 ],
                        dest  => "run",
                       },
                       {
                        files => [
                                  "tests/albtest",
                                  "tests/[a-z]*.so",
                                  "tests/valgrind*",
                                 ],
                        dest  => "run",
                       },
                       {
                        files => [
                                  "tests/*.perftest",
                                  "tests/[A-Z]*.so",
                                  "tests/dropCaches",
                                 ],
                        dest  => "tests",
                       },
                       {
                        files => ["tests/symcheck.d/*"],
                        dest  => "run/symcheck.d",
                       },
                      );

our %PROPERTIES
  = (
     # @ple The platform directory
     platformDir  => "userLinux",
     # @ple The albtest arguments
     unitTestName => "",
    );

#############################################################################
##
sub tear_down {
  my ($self) = assertNumArgs(1, @_);
  if (defined($self->{clientNames})) {
    # XXX These commands are intended to help track down why we are getting
    #     incomplete core files, and is part of investigating ALB-2960.
    my $alb2960Command = join("; ",
                              "ls -l $self->{runDir}",
                              "df $self->{runDir}");
    runCommand($self->{clientNames}[0], $alb2960Command);
    runCommand($self->{clientNames}[0], "sudo rm -fr /u1/zubenelgenubi*");
    runCommand($self->{clientNames}[0],
               "sudo pkill -QUIT '^albtest\$'");
    runCommand($self->{clientNames}[0],
               "sudo pkill -KILL '^albtest\$'");
    # XXX This command is intended to help track down why we are getting
    #     incomplete core files, and is part of investigating ALB-2960.
    runCommand($self->{clientNames}[0], $alb2960Command);
  }
  $self->SUPER::tear_down();
}

#############################################################################
##
sub testRunner {
  my ($self) = assertNumArgs(1, @_);
  my $testHost = $self->{clientNames}[0];
  my $udsDir = "src/c++/uds";
  my $buildDir = makeFullPath($self->{topDir}, $udsDir, $self->{platformDir});
  my $testDir  = makeFullPath($self->{workDir}, "tests");

  Permabit::FileCopier->new(mainBase     => $buildDir,
                            machine      => $testHost,
                            targetBinDir => $self->{workDir},
                            sourceFiles  => \@USER_TEST_FILES,
                           )->copySrcFiles();

  # XXX to help track down why we're getting weird permission failures
  # intermittently in Jenkins
  runCommand($testHost, "ls -ld /u1");
  runCommand($testHost, "ls -l /u1");

  my $logFile = makeFullPath($self->{runDir}, "albtest.log");
  my $env = join(" ", ("ALBTEST_DIR=$testDir",
                       "UDS_LOGFILE=$logFile"));
  my $cmd = $self->getTestCommand();
  assertCommand($testHost, join(" && ", "umask 0", "ulimit -c unlimited",
                                "cd $self->{runDir}", "env $env $cmd"));
}

#############################################################################
# Get the command that actually runs the test.  This may be overridden in a
# subclass.
#
# @return the command that runs the test.
##
sub getTestCommand {
  my ($self) = assertNumArgs(1, @_);
  return "./albtest $self->{unitTestName}";
}

1;
