#!/usr/bin/perl

##
# This is a Delta Master Index calculator for Albireo 3.0
##

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Cwd qw(cwd);
use FindBin;
use lib (
  cwd(),
  $FindBin::RealBin,
  "${FindBin::RealBin}/../lib",
  (defined($ENV{PERMABIT_PERL_PATH}))
           ? split('\s*,\s*', $ENV{PERMABIT_PERL_PATH})
           : "${FindBin::RealBin}/../commonlib",
);


use Permabit::Assertions qw(assertNumArgs);

my $meanDelta = parseArg($ARGV[0], 4096);
my $collisionPayload = parseArg($ARGV[1], 256);
my $medianDelta = sprintf("%.0f", log(2) * $meanDelta);
my $baseBits = baseBits($medianDelta + 1);
my $expected = $baseBits + 2 * exp(($medianDelta - $meanDelta) / $meanDelta);
my $collisionRate = (1 - exp(-1 / $meanDelta)) / 2;
$expected += 256 * $collisionRate;

print STDOUT "Mean Delta = $meanDelta\n";
print STDOUT "Median Delta = $medianDelta\n";
print STDOUT "Collision rate = $collisionRate\n";
print STDOUT "Collision payload = $collisionPayload bits\n";
print STDOUT "Expect to use $expected bits for deltas and collisions\n";
exit(0);

sub parseArg {
  my ($arg, $default) = assertNumArgs(2, @_);
  if (!defined($arg)) {
    return $default;
  }
  if ($arg =~ /^\d+$/) {
    return 0 + $arg;
  }
  print STDERR <<HERE;
Syntax: dmiAlb <delta> <collision>

where <delta> is the mean delta and <collision> is the number of bits added
to the record by a collision.  The defaults are 4096 and 256.
HERE
  exit(1);
}

sub baseBits {
  my ($value) = assertNumArgs(1, @_);
  for (my $bits = 0;; $bits++) {
    if ($value <= (1 << $bits)) {
      return $bits;
    }
  }
}
