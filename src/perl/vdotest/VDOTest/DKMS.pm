##
# Test installing a VDO tarball with Dynamic Kernel Module Support (DKMS)
#
# $Id$
##
package VDOTest::DKMS;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Utils qw(findFile);
use Permabit::Version qw($VDO_MARKETING_VERSION $VDO_MODNAME);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

###############################################################################
# @inherit
##
sub listSharedFiles {
  my ($self) = assertNumArgs(1, @_);
  my $vdoTGZ = $self->getTGZNameForVersion($VDO_MARKETING_VERSION);
  return ($self->SUPER::listSharedFiles(), $vdoTGZ);
}

#############################################################################
# Test installation with DKMS
##
sub testInstall {
  my ($self) = assertNumArgs(1, @_);
  my ($name, $ver) = ($VDO_MODNAME, $VDO_MARKETING_VERSION);
  my $machine = $self->getUserMachine();
  my $host = $machine->getName();

  my $tarFile = findFile("$name-$ver.tgz", [$self->{binaryDir}]);
  $machine->assertExecuteCommand("(cd /usr/src; sudo tar zxvf $tarFile)");
  # we just created the /usr/src/$name-$ver directory
  $machine->assertExecuteCommand("sudo dkms add -m $name -v $ver");
  $machine->assertExecuteCommand("sudo dkms build -m $name -v $ver");
  $machine->assertExecuteCommand("sudo dkms install -m $name -v $ver");
  $self->manualWaitPoint("DKMSinstalled",
                         "$name dkms package has been installed on: $host");
  $self->runTearDownStep(sub {
    $machine->assertExecuteCommand("sudo modinfo $name");
    $machine->assertExecuteCommand("sudo modprobe $name");
    $machine->assertExecuteCommand("sudo modprobe -r $name");
    $machine->assertExecuteCommand("sudo dkms status -m $name");
  });
  $machine->assertExecuteCommand("sudo dkms remove -m $name -v $ver --all");
  $machine->runSystemCmd("sudo rm -r /usr/src/$name-$ver");
}

1;
