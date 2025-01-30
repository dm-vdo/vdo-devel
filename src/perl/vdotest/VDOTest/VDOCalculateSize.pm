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
use YAML;

use base qw(VDOTest);
use strict;
use warnings FATAL => qw(all);

my $log = Log::Log4perl->get_logger(__PACKAGE__);

my %expectedResult
  = (
     "Volume characteristics in blocks" => {
       "Slab size" => "1048576"
     },
     "VDO metadata" => {
       "Slab reference count usage" => "77082624",
     },
     "Storage usage" => {
       "Total physical usable size" => "309295575040",
       "Dedupe window" => "1099511627776",
       "Total block map pages" => "1355763712",
     },
     "VDO in memory usage" => {
       "Block map cache"  => "134217728",
       "Forest memory usage" => "1683456",
       "UDS index size" => "11193331712",
     },
   );

my %runProperties
  = (
    binary => undef,
    machine => undef,
    );

sub compareResult {
  my($self, $cmdResult, $myExpectedResult) = assertNumArgs(3, @_);
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1}}) {
      my $expRes = (defined($myExpectedResult->{$key1}->{$key2})) ?
                   $myExpectedResult->{$key1}->{$key2} :
                   $expectedResult{$key1}{$key2};
      assertEq($expRes, $cmdResult->{$key1}->{$key2}, $key2);
    }
  }
}

sub executeTest {
  my($self, $args, $myExpectedResult) = assertNumArgs(3, @_);
  my $machine = $runProperties{machine};

  $args->{'binary'} = $runProperties{binary};
  my $command = Permabit::CommandString::VDOCalculateSize->new($self, $args);
  $machine->assertExecuteCommand("$command");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $self->compareResult($sizeYaml, $myExpectedResult);
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
  my $myExpectedResult = {
    "Storage usage" => {
      "Total physical usable size" => "307939823616",
      "Total block map pages" => "2711515136",
    },
    "VDO in memory usage" => {
      "Forest memory usage" => "3354624",
    },
  };
  $self->executeTest($args, $myExpectedResult);
}

######################################################################
##
sub test3TLogicalSize {
  my ($self) = assertNumArgs(1, @_);
  my $args = {
    "logical-size" => "3T",
    "slab-bits" => "20",
  };
  my $myExpectedResult = {
    "Storage usage" => {
      "Total physical usable size" => "306584080384",
      "Total block map pages" => "4067258368",
    },
    "VDO in memory usage" => {
      "Forest memory usage" => "5021696",
    },
  };
  $self->executeTest($args, $myExpectedResult);  
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
  my $myExpectedResult = {
    "VDO metadata" => {
      "Slab reference count usage" => "56033280",
    },
    "Storage usage" => {
      "Total physical usable size" => "221754777600",
      "Total block map pages" => "4067258368",
      "Dedupe window" => "10995116277760",
    },
    "VDO in memory usage" => {
      "Forest memory usage" => "5021696",
      "UDS index size" => "96062033920",
   },
  };
  $self->executeTest($args, $myExpectedResult);  
}

######################################################################
##
sub testSmall25Index {
  my ($self) = assertNumArgs(1, @_);
  my $args = {
    "index-memory-size" => "0.25",
    "slab-bits" => "20",
  };
  my $myExpectedResult = {
    "VDO metadata" => {
      "Slab reference count usage" => "79171584",
    },
    "Storage usage" => {
      "Total physical usable size" => "317703278592",
      "Total block map pages" => "1355763712",
      "Dedupe window" => "256",
    },
    "VDO in memory usage" => {
      "UDS index size" => "2781704192",
    },
  };
  $self->executeTest($args, $myExpectedResult);  
}

sub testSlabSize1G {
  my ($self) = assertNumArgs(1, @_);
  my $args = {
    "logical-size" => "2T",
    "slab-size" => "1G",
  };
  my $myExpectedResult = {
    "Volume characteristics in blocks" => {
      "Slab size" => "262144"
    },
    "VDO metadata" => {
       "Slab reference count usage" => "77082624",
    },
    "Storage usage" => {
      "Total physical usable size" => "307740725248",
      "Total block map pages" => "2711515136",
      "Dedupe window" => "1099511627776",
    },
    "VDO in memory usage" => {
      "UDS index size" => "11193331712",
      "Forest memory usage" => "3354624",
    },
  };
  $self->executeTest($args, $myExpectedResult);  
}

1;
