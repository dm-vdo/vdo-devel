##
# Perl object that represents a set of process or thread statistics read
# from /proc/$pid/stat or /proc/$pid/task/$tid/stat
#
# $Id$
##
package Permabit::Statistics::Proc;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);
use List::MoreUtils qw(zip);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertDefined
  assertMinArgs
  assertMinMaxArgs
  assertNumArgs
);

use base qw(Permabit::Statistics);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %TITLE_MAP = (
                 getCpuBusy => "cpuBusy",
                );

my %TYPE_MAP = (
                pid     => "constant",
                name    => "constant",
                state   => "state",
                ppid    => "constant",
                pgrp    => "constant",
                session => "constant",
                tty_nr  => "constant",
                tpgid   => "constant",
                flags   => "state",
                minflt  => "counter",
                cminflt => "counter",
                majflt  => "counter",
                cmajflt => "counter",
                utime   => "counter",
                stime   => "counter",
                cutime  => "counter",
                cstime  => "counter",
                ticks   => "constant",
               );

my %UNITS_MAP = (
                 getCpuBusy => "Busy Fraction",
                 minflt     => "Faults",
                 cminflt    => "Faults",
                 majflt     => "Faults",
                 cmajflt    => "Faults",
                 utime      => "Ticks",
                 stime      => "Ticks",
                 cutime     => "Ticks",
                 cstime     => "Ticks",
                );

#############################################################################
# @paramList{new}
#
# Note that dclone is not used for the properties; the *Map hashes are
# intended to be shared.
my %PROPERTIES
  = (
     # @ple Process ID.
     pid     => undef,
     # @ple Name.  For a process, this is the filename of the executable.
     #      For a kernel thread, this is the thread name.
     name    => 0,
     # @ple One character from the string "RSDZTW" where R is running, S is
     #      sleeping in an interruptible wait, D is waiting in
     #      uninterruptible disk sleep, Z is zombie, T is traced or stopped
     #      (on a signal), and W is paging.
     state   => 0,
     # @ple Process ID of the parent.
     ppid    => 0,
     # @ple Process group ID of the process.
     pgrp    => 0,
     # @ple Session ID of the process.
     session => 0,
     # @ple The controlling terminal of the process.  (The minor device
     #      number is contained in the combination of bits 31 to 20 and 7
     #      to 0; the major device number is in bits 15 t0 8.)
     tty_nr  => 0,
     # @ple The ID of the foreground process group of the controlling
     #      terminal of the process.
     tpgid   => 0,
     # @ple The kernel flags word of the process.  For bit meanings, see
     #      the PF_* defines in <linux/sched.h>.  Details depend on the
     #      kernel version.
     flags   => 0,
     # @ple The number of minor faults the process has made which have not
     #      required loading a memory page from disk.
     minflt  => 0,
     # @ple The number of minor faults that the process's waited for
     #      children have made.
     cminflt => 0,
     # @ple The number of major faults the process has made which have
     #      required loading a memory page from disk.
     majflt  => 0,
     # @ple The number of major faults that the process's waited for
     #      children have made.
     cmajflt => 0,
     # @ple Amount of time that this process has been scheduled in user
     #      mode, measured in clock ticks.  This includes guest time,
     #      guest_time (time spent running a virtual CPU, see below), so
     #      that applications that are not aware of the guest time field do
     #      not lose that time from their calculations.
     utime   => 0,
     # @ple Amount of time that this process has been scheduled in kernel
     #      mode, measured in clock ticks.
     stime   => 0,
     # @ple Amount of time that this process's waited for children have
     #      been scheduled in user mode, measured in clock ticks.  This
     #      includes guest time, cguest_time (time spent running a virtual
     #      CPU, see below).
     cutime  => 0,
     # @ple Amount of time that this process's waited for children have
     #      been scheduled in kernel mode, measured in clock ticks.
     cstime  => 0,

     # @ple Clock ticks per second.  Divide by this to convert utime, stime,
     #      cutime and cstime to seconds.
     ticks   => undef,

     # @ple the hash mapping field names to titles
     titleMap    => \%TITLE_MAP,
     # @ple the hash mapping field names to value types.
     typeMap     => \%TYPE_MAP,
     # @ple the hash mapping field names to units
     unitsMap    => \%UNITS_MAP,
    );
##

#############################################################################
# Creates a C<Permabit::Statistics::Proc>. C<new> optionally takes arguments,
# in the form of key-value pairs.
#
# @params{new}
#
# @return a new C<Permabit::DiskStats>
##
sub new {
  my $invocant = shift;
  my $self = $invocant->SUPER::new(%PROPERTIES,
                                   # Overrides previous values
                                   @_);
  assertDefined($self->{ticks});
  return $self;
}

#############################################################################
# Get the fraction of time that the cpu is busy.
#
# @return fraction of time that the cpu is busy.
##
sub getCpuBusy {
  my ($self) = assertNumArgs(1, @_);
  return (($self->{utime} + $self->{stime})
          / $self->{ticks} / $self->{duration});
}

#############################################################################
# @inherit
##
sub logStats {
  my ($self, $label, $prefix) = assertMinMaxArgs([""], 2, 3, @_);
  $self->SUPER::logStats($label, $prefix);
  $log->info("$prefix  pid      => $self->{pid}");
  $log->info("$prefix  name     => $self->{name}");
  if (defined($self->{state})) {
    $log->info("$prefix  state    => $self->{state}");
  }
  $log->info("$prefix  ppid     => $self->{ppid}");
  $log->info("$prefix  pgrp     => $self->{pgrp}");
  $log->info("$prefix  session  => $self->{session}");
  $log->info("$prefix  tty_nr   => $self->{tty_nr}");
  $log->info("$prefix  tpgid    => $self->{tpgid}");
  if (defined($self->{flags})) {
    $log->info("$prefix  flags    => $self->{flags}");
  }
  $log->info("$prefix  minflt   => $self->{minflt}");
  $log->info("$prefix  majflt   => $self->{majflt}");
  $log->info("$prefix  cminflt  => $self->{cminflt}");
  $log->info("$prefix  cmajflt  => $self->{cmajflt}");
  $log->info("$prefix  utime    => $self->{utime}");
  $log->info("$prefix  stime    => $self->{stime}");
  $log->info("$prefix  cutime   => $self->{cutime}");
  $log->info("$prefix  cstime   => $self->{cstime}");
  if (defined($self->{duration})) {
    $log->info("$prefix  cpuBusy => " . $self->getCpuBusy());
  }
}

1;
