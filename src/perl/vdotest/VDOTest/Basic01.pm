##
# Basic VDO functional test
#
# $Id$
##
package VDOTest::Basic01;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEq
  assertNumArgs
  assertTrue
);
use Permabit::Constants;

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES
  = (
     # @ple Use a VDO device
     deviceType    => "lvmvdo",
     # @ple VDO slab bit count
     slabBits      => $SLAB_BITS_SMALL,
     # @ple This test requires a filesystem
     useFilesystem => 1,
    );
##

#############################################################################
##
sub testBasic01 {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $fs = $self->getFileSystem();

  my $mountPoint = $fs->getMountDir();
  my $file1 = "$mountPoint/foo1";
  $machine->runSystemCmd("echo Hello World > $file1");

  my $dir2 = $fs->makeSubdir("dir2");
  my $file2 = "$dir2/foo2";
  $machine->runSystemCmd("cp $file1 $file2");

  my $file3 = "$mountPoint/foo3";
  $machine->runSystemCmd("cp $file1 $file3");

  $machine->dropCaches();
  assertEq("Hello World\n", $machine->cat($file1));
  assertEq("Hello World\n", $machine->cat($file2));

  $fs->unmount();
  $device->stop();
  my $logCursor = $machine->getKernelJournalCursor();
  $device->start();
  $fs->mount();

  assertEq("Hello World\n", $machine->cat($file3));
  my $pattern = "VDO commencing normal operation";
  assertTrue($machine->searchKernelJournalSince($logCursor, $pattern));
}

1;
