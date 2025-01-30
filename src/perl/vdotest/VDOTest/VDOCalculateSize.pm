##
# Basic vdoCalculateSize tests
#
# $Id$
##
package VDOTest::VDOCalculateSize;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEq
  assertNumArgs
);
use Permabit::Constants;
use YAML;
use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);
my %expectedResult;
$expectedResult{'VDO'}{'Slab Size'}="1048576";
$expectedResult{'VDO meta data'}{'UDS Index Size'}="21469MB";
$expectedResult{'VDO Usage'}{'Usable Size'}="277GB";
$expectedResult{'VDO Usage'}{'Dedup Window'}="2048GB";

######################################################################
# Test the basic execution of vdoCalculatSize.
##
sub testBasicExecution {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my $args = "--slabBits 20 --physicalSize=300G " .
             "--logicalSize=1T --indexMemorySize=2";
  $machine->runSystemCmd("$cmd $args");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $args\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}
1;
