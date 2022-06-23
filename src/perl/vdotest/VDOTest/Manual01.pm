##
# Manual VDO test.
#
# $Id$
##
package VDOTest::Manual01;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::Utils qw(waitForInput);

use base qw(VDOTest);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

##
# @paramList{getProperties}
our %PROPERTIES = (
  # @ple Use a VDO device
  deviceType    => "lvmvdo",
  # @ple This test requires a filesystem
  useFilesystem => 1,
);
##

#############################################################################
##
sub testManual01 {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $hostName = $machine->getName();
  my $devicePath = "/dev/mapper/$device->{deviceName}";

  my $info = "$hostName: device $devicePath";
  if ($self->{useFilesystem}) {
    my $fs = $self->getFileSystem();
    my $mountPoint = $fs->getMountDir();
    $info = "$hostName: VDO device $devicePath mounted at $mountPoint";
  }

  my %commands = ( 'quit'    => "quit this test",
		   'help'    => "print this message",
		   'info'    => "repeat setup info",
		   'flush'   => "flush caches",
		   'log'     => "log arguments at info level",
		 );

  my $output = *STDERR;
  if (! -t STDERR and open(OUTPUT,"+>/dev/tty")) {
    $output = *OUTPUT;
  }
  print $output "\n", $info, "\n";
  while (1) {
    my $response = waitForInput(">>> ");
    if (!defined($response)) {
      last;
    }
    chomp($response);
    if ($response eq '') {
      next;
    }
    my @command = split ' ', $response;
    if (!defined($commands{$command[0]})) {
      print $output "unknown command '$command[0]', 'help' for help\n";
    } elsif ($command[0] eq 'quit') {
      last;
    } elsif ($command[0] eq 'help') {
      print $output "command\tdescription\n";
      print $output "=======\t==============================================\n";
      while (my ($command, $desc) = each(%commands)) {
	print $output "$command\t$desc\n";
      }
    } elsif ($command[0] eq 'info') {
      print $output $info, "\n";
    } elsif ($command[0] eq 'flush') {
      $machine->dropCaches();
    } elsif ($command[0] eq 'log') {
      $log->info(join(' ', @command[1 .. $#command]));
    } else {
      croak("unimplemented Manual01 test command $command[0]");
    }
  }
  print $output "test stopping\n";
  close OUTPUT;
}

1;
