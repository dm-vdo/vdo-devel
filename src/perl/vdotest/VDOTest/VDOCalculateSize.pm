##
# Basic vdoCalculateSize tests
#
# $Id$
##
package VDOTest::VDOCalculateSize;

use English qw(-no_match_vars);
use Log::Log4perl;
use Permabit::Assertions qw(
  assertEq
  assertNumArgs
);
use Permabit::CommandString::VDOCalculateSize;
use Permabit::Constants;
use Storable qw(dclone);
use YAML;

use base qw(VDOTest);
use strict;
use warnings FATAL => qw(all);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %expectedResultBase
  = (
     "Volume characteristics in blocks" => {
       "Physical blocks" => "78643200",
       "Logical blocks" => "268435456",
       "Slab size" => "1048576",
       "Slab count" => "72",
     },
     "Storage usage" => {
       "Total physical usable size" => "309295575040",
       "Dedupe window" => "1099511627776",
       "Total block map pages" => "1355763712",
     },
     "VDO in memory usage" => {
       "Total in memory usage" => "11406315520",
       "Block map cache"  => "134217728",
       "Forest memory usage" => "1683456",
       "UDS index size" => "11193331712",
     },
     "VDO metadata" => {
       "Slab reference count usage" => "77082624",
     },
   );

my %runProperties
  = (
    binary => undef,
    machine => undef,
    );

sub mergeWithBase {
  my($self, $currentRunExpectedResult) = assertNumArgs(2, @_);

  foreach my $key1 (keys %expectedResultBase) {
    foreach my $key2 (keys %{$expectedResultBase{$key1}}) {
      if (!defined($currentRunExpectedResult->{$key1}->{$key2})) {
          $currentRunExpectedResult->{$key1}->{$key2} =
             $expectedResultBase{$key1}{$key2};
      }
    }
  }
}

sub compareResults {
  my($self, $cmdResult, $expectedResult) = assertNumArgs(3, @_);
  foreach my $key1 (keys %$expectedResult) {
    foreach my $key2 (keys %{$expectedResult->{$key1}} ) {
      assertEq($expectedResult->{$key1}->{$key2},
               $cmdResult->{$key1}->{$key2}, $key2);
    }
  }
}

sub executeTest {
  my($self, $args, $runExpectedResult) = assertNumArgs(3, @_);
  my $machine = $runProperties{machine};

  $args->{'binary'} = $runProperties{binary};
  my $command = Permabit::CommandString::VDOCalculateSize->new($self, $args);
  $machine->assertExecuteCommand("$command");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $self->compareResults($sizeYaml, $runExpectedResult);
}

###############################################################################
# @inherit
##
sub set_up {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::set_up();
  my $device  = $self->getDevice();

  $runProperties{machine} = $device->getMachine();
  $runProperties{binary} => $self->findBinary("vdoCalculateSize"),
}

sub testBasicExecution {
  my ($self) = assertNumArgs(1, @_);
  my $args = {
    "slab-bits" => "20",
  };
  $self->executeTest($args, {});
}

sub test2TLogicalSize {
  my ($self) = assertNumArgs(1, @_);
  my $args = {
    "logical-size" => "2T",
    "slab-bits" => "20",
  };
  my $currentTestExpectedResult = {
    "Volume characteristics in blocks" => {
      "Logical blocks" => "536870912",
    },
    "Storage usage" => {
      "Total physical usable size" => "307939823616",
      "Total block map pages" => "2711515136",
    },
    "VDO in memory usage" => {
      "Total in memory usage" => "11407986688",
      "Forest memory usage" => "3354624",
    },
  };

  $self->mergeWithBase($currentTestExpectedResult);
  $self->executeTest($args, $currentTestExpectedResult);
}

######################################################################
##
sub test3TLogicalSize {
  my ($self) = assertNumArgs(1, @_);
  my $args = {
    "logical-size" => "3T",
    "slab-bits" => "20",
  };
  my $currentTestExpectedResult = {
    "Volume characteristics in blocks" => {
      "Logical blocks" => "805306368",
    },
    "Storage usage" => {
      "Total physical usable size" => "306584080384",
      "Total block map pages" => "4067258368",
    },
    "VDO in memory usage" => {
      "Total in memory usage" => "11409653760",
      "Forest memory usage" => "5021696",
    },
  };

  $self->mergeWithBase($currentTestExpectedResult);
  $self->executeTest($args, $currentTestExpectedResult);
}

######################################################################
##
sub test3TLogicalSizeSparse {
  my ($self) = assertNumArgs(1, @_);
  my $args = {
    "logical-size" => "3T",
    "sparse-index" => 1,
    "slab-bits" => "20",
  };
  my $currentTestExpectedResult = {
    "Volume characteristics in blocks" => {
      "Logical blocks" => "805306368",
    },
    "Storage usage" => {
      "Total physical usable size" => "306584080384",
      "Total block map pages" => "4067258368",
    },
    "VDO in memory usage" => {
      "Total in memory usage" => "11409653760",
      "Forest memory usage" => "5021696",
    },
  };

  $self->mergeWithBase($currentTestExpectedResult);
  $self->executeTest($args, $currentTestExpectedResult);
}

######################################################################
##
sub testSmall25Index {
  my ($self) = assertNumArgs(1, @_);
  my $args = {
    "index-memory-size" => "0.25",
    "slab-bits" => "20",
  };
  my $currentTestExpectedResult = {
    "Volume characteristics in blocks" => {
      "Logical blocks" => "268435456",
      "Slab size" => "1048576",
      "Slab count" => "74",
    },
    "Storage usage" => {
      "Total physical usable size" => "317703278592",
      "Total block map pages" => "1355763712",
      "Dedupe window" => "274877906944",
    },
    "VDO in memory usage" => {
      "Total in memory usage" => "2996776960",
      "Forest memory usage" => "1683456",
      "UDS index size" => "2781704192",
    },
    "VDO metadata" => {
      "Slab reference count usage" => "79171584",
    },
  };

  $self->mergeWithBase($currentTestExpectedResult);
  $self->executeTest($args, $currentTestExpectedResult);
}

sub testSlabSize1G {
  my ($self) = assertNumArgs(1, @_);
  my $args = {
    "logical-size" => "2T",
    "slab-size" => "1G",
  };
  my $currentTestExpectedResult = {
    "Volume characteristics in blocks" => {
      "Logical blocks" => "536870912",
      "Slab size" => "262144",
      "Slab count" => "289",
    },
    "Storage usage" => {
      "Total physical usable size" => "307740725248",
      "Total block map pages" => "2711515136",
    },
    "VDO in memory usage" => {
      "Total in memory usage" => "11407986688",
      "Forest memory usage" => "3354624",
    },
  };

  $self->mergeWithBase($currentTestExpectedResult);
  $self->executeTest($args, $currentTestExpectedResult);
}

1;
