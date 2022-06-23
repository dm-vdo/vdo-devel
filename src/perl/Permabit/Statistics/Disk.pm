##
# Perl object that represents a set of disk statistics read from
# /proc/diskstats or /sys/..../stat
#
# $Id$
##
package Permabit::Statistics::Disk;

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
use Permabit::Constants qw($SECTOR_SIZE);

use base qw(Permabit::Statistics);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %TITLE_MAP = (
                 getIOBusy         => "busyRate",
                 getReadMergeRate  => "readMergeRate",
                 getReadRate       => "readRate",
                 getTotalReads     => "totalReads",
                 getTotalWrites    => "totalWrites",
                 getWriteMergeRate => "writeMergeRate",
                 getWriteRate      => "writeRate",
                );

my %TYPE_MAP = (
                name                   => "constant",
                readCompleted          => "counter",
                readMerged             => "counter",
                readSectors            => "counter",
                readMilliseconds       => "counter",
                writeCompleted         => "counter",
                writeMerged            => "counter",
                writeSectors           => "counter",
                writeMilliseconds      => "counter",
                ioInProgress           => "snapshot",
                ioMilliseconds         => "counter",
                ioWeightedMilliseconds => "counter",
               );

my %UNITS_MAP = (
                 getIOBusy         => "Busy Fraction",
                 getReadMergeRate  => "Read Merge Fraction",
                 getReadRate       => "Bytes/Second",
                 getTotalReads     => "Count",
                 getTotalWrites    => "Count",
                 getWriteMergeRate => "Write Merge Fraction",
                 getWriteRate      => "Bytes/Second",
                 ioInProgress      => "Count",
                 ioMilliseconds    => "Milliseconds",
                 readCompleted     => "Count",
                 readMerged        => "Count",
                 readMilliseconds  => "Milliseconds",
                 readSectors       => "Sectors",
                 writeCompleted    => "Count",
                 writeMerged       => "Count",
                 writeMilliseconds => "Milliseconds",
                 writeSectors      => "Sectors",
                );

#############################################################################
# @paramList{new}
#
# Note that dclone is not used for the properties; the *Map hashes are
# intended to be shared.
my %PROPERTIES
  = (
     # @ple name of the device (as known inside the linux kernel)
     name                   => undef,
     # @ple The total number of reads completed successfully.
     readCompleted          => 0,
     # @ple The number of reads merged.  Reads which are adjacent to each
     #      other may be merged for efficiency.  Thus two 4K reads may
     #      become one 8K read before it is ultimately handed to the disk,
     #      and so it will be counted (and queued) as only one I/O.  This
     #      number lets you know how often this was done.
     readMerged             => 0,
     # @ple The total number of sectors read successfully.
     readSectors            => 0,
     # @ple The number of milliseconds spent reading.  This is the total
     #      number of milliseconds spent by all reads (as measured from
     #      __make_request() to end_that_request_last()).
     readMilliseconds       => 0,
     # @ple The total number of writes completed successfully.
     writeCompleted         => 0,
     # @ple The number of writes merged.  Writes which are adjacent to each
     #      other may be merged for efficiency.  Thus two 4K writes may
     #      become one 8K write before it is ultimately handed to the disk,
     #      and so it will be counted (and queued) as only one I/O.  This
     #      number lets you know how often this was done.
     writeMerged            => 0,
     # @ple The total number of sectors written successfully.
     writeSectors           => 0,
     # @ple The number of milliseconds spent writing.  This is the total
     #      number of milliseconds spent by all writes (as measured from
     #      __make_request() to end_that_request_last()).
     writeMilliseconds      => 0,
     # @ple The number of I/Os currently in progress.  This is the only
     #      field that should go to zero.  It is incremented as requests
     #      are given to appropriate struct request_queue and decremented
     #      as they finish.
     ioInProgress           => 0,
     # @ple The number of milliseconds spent doing I/Os.  This field is
     #      increases so long as ioInProgress is nonzero.
     ioMilliseconds         => 0,
     # @ple The weighted number of milliseconds spent doing I/Os.  This
     #      field is incremented at each I/O start, I/O completion, I/O
     #      merge, or read of these stats by the number of I/Os in progress
     #      times the number of milliseconds spent doing I/O since the last
     #      update of this field.  This can provide an easy measure of both
     #      I/O completion time and the backlog that may be accumulating.
     ioWeightedMilliseconds => 0,

     # @ple the hash mapping field names to titles
     titleMap    => \%TITLE_MAP,
     # @ple the hash mapping field names to value types.
     typeMap     => \%TYPE_MAP,
     # @ple the hash mapping field names to units
     unitsMap    => \%UNITS_MAP,
    );
##

#############################################################################
# Creates a C<Permabit::Statistics::Disk>. C<new> optionally takes arguments,
# in the form of key-value pairs.
#
# @params{new}
#
# @return a new C<Permabit::Statistics::Disk>
##
sub new {
  my $invocant = shift;
  my $self = $invocant->SUPER::new(%PROPERTIES,
                                   # Overrides previous values
                                   @_);
  assertDefined($self->{name});
  return $self;
}

#############################################################################
# Creates a C<Permabit::Statistics::Disk> by extracting the disk statistics
# associated with a major/minor device number from a copy of /proc/diskstats.
#
# @param  diskstats  the contents of /proc/diskstats
# @param  major      the device major number
# @param  minor      the device minor number
# @oparam rest       the rest of the initial values
##
sub newByMajorMinor {
  my ($invocant, $diskstats, $major, $minor, %rest) = assertNumArgs(4, @_);
  if ($diskstats !~ m/^\s+$major\s+$minor\s+(.+)/m) {
    croak("Device $major, $minor is not in /proc/diskstats\n$diskstats");
  }
  my $stat = $1;
  # Keys must be in the exact order the values appear in $stat.
  my @statKeys = qw(
                     name
                     readCompleted
                     readMerged
                     readSectors
                     readMilliseconds
                     writeCompleted
                     writeMerged
                     writeSectors
                     writeMilliseconds
                     ioInProgress
                     ioMilliseconds
                     ioWeightedMilliseconds
                  );
  my @deviceStats = (split(qr/\s+/, $stat))[0..$#statKeys];
  return $invocant->new(zip(@statKeys, @deviceStats), %rest);
}

#############################################################################
# Compute the fraction of the time that the IO device is busy
#
# @return the fraction of the time that the IO device is busy
##
sub getIOBusy {
  my ($self) = assertNumArgs(1, @_);
  return $self->{ioMilliseconds} / 1000 / $self->{duration};
}

#############################################################################
# Compute the read rate of the IO device
#
# @param the read rate in bytes per second
##
sub getReadRate {
  my ($self) = assertNumArgs(1, @_);
  return $SECTOR_SIZE * $self->{readSectors} / $self->{duration};
}

#############################################################################
# Compute the total number of reads
#
# @return the total number of reads
##
sub getTotalReads {
  my ($self) = assertNumArgs(1, @_);
  return $self->{readCompleted} + $self->{readMerged};
}

#############################################################################
# Compute the fraction of reads merged
#
# @return the fraction of reads that were merged
##
sub getReadMergeRate {
  my ($self) = assertNumArgs(1, @_);
  my $total = $self->getTotalReads();
  if ($total == 0) {
    return 0;
  }
  return $self->{readMerged} / $total;
}

#############################################################################
# Compute the total number of writes
#
# @param the total number of writes
##
sub getTotalWrites {
  my ($self) = assertNumArgs(1, @_);
  return $self->{writeCompleted} + $self->{writeMerged};
}

#############################################################################
# Compute the write rate of the IO device
#
# @param the write rate in bytes per second
##
sub getWriteRate {
  my ($self) = assertNumArgs(1, @_);
  return $SECTOR_SIZE * $self->{writeSectors} / $self->{duration};
}

#############################################################################
# Compute the fraction of writes merged
#
# @return the fraction of writes that were merged
##
sub getWriteMergeRate {
  my ($self) = assertNumArgs(1, @_);
  my $total = $self->getTotalWrites();
  if ($total == 0) {
    return 0;
  }
  return $self->{writeMerged} / $total;
}

#############################################################################
# @inherit
##
sub logStats {
  my ($self, $label, $prefix) = assertMinMaxArgs([""], 2, 3, @_);
  $self->SUPER::logStats($label, $prefix);
  $log->info("$prefix  name                   => $self->{name}");
  $log->info("$prefix  readCompleted          => $self->{readCompleted}");
  $log->info("$prefix  readMerged             => $self->{readMerged}");
  $log->info("$prefix  readSectors            => $self->{readSectors}");
  $log->info("$prefix  readMilliseconds       => $self->{readMilliseconds}");
  $log->info("$prefix  writeCompleted         => $self->{writeCompleted}");
  $log->info("$prefix  writeMerged            => $self->{writeMerged}");
  $log->info("$prefix  writeSectors           => $self->{writeSectors}");
  $log->info("$prefix  writeMilliseconds      => $self->{writeMilliseconds}");
  if (defined($self->{ioInProgress})) {
    $log->info("$prefix  ioInProgress           => $self->{ioInProgress}");
  }
  $log->info("$prefix  ioMilliseconds         => $self->{ioMilliseconds}");
  $log->info("$prefix  ioWeightedMilliseconds => "
             . $self->{ioWeightedMilliseconds});
  if (defined($self->{duration})) {
    $log->info("$prefix  busyRate               => " . $self->getIOBusy());
    $log->info("$prefix  readRate               => " . $self->getReadRate());
    $log->info("$prefix  totalReads             => " . $self->getTotalReads());
    $log->info("$prefix  totalWrites            => "
               . $self->getTotalWrites());
    $log->info("$prefix  writeRate              => " . $self->getWriteRate());
    $log->info("$prefix  readMergeRate          => "
               . $self->getReadMergeRate());
    $log->info("$prefix  writeMergeRate         => "
               . $self->getWriteMergeRate());
  }
}

1;
