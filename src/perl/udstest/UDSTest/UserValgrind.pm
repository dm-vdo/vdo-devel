##
# Base class for tests which run user mode linux valgrind tests.  Runs each
# test in its own testcase.
#
# $Id$
##
package UDSTest::UserValgrind;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);

use base qw(UDSTest::UserSeparated);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

#############################################################################
# @inherit
##
sub getTestCommand {
  my ($self) = assertNumArgs(1, @_);
  return "./valgrind_albtest $self->{unitTestName}";
}

1;
