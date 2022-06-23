##
# Mix-in containing the build rule for the VDO tree.
#
# $Id$
##
package NightlyBuildType::VDOBuilder;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);

use Log::Log4perl;

use Permabit::Assertions qw(assertNumArgs);
use Permabit::Utils qw(makeFullPath);

use base qw(NightlyBuildType);

# Log4perl Logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

######################################################################
# @inherit
##
sub getP4ViewImplementation {
  my ($self) = assertNumArgs(1, @_);
  return "    $self->{depotRoot}/src/...	//$ENV{P4CLIENT}/src/...\n";
}

######################################################################
# @inherit
##
sub buildAllImplementation {
  my ($self) = assertNumArgs(1, @_);
  $self->build("Building_All", "make all", $self->{srcRoot});

  # Don't bother running unit tests and making the tarball if this is
  # a Checkin build type
  if (ref($self) =~ /Checkin/) {
    return;
  }

  $self->build("Building_Extra", "make checkin",
               makeFullPath($self->{srcRoot}, 'c++/vdo/tests'));
}

1;
