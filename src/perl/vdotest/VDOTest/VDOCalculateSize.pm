##
# Basic vdoCalculateSize tests
#
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

######################################################################
# Test the basic execution of vdoCalculatSize.
##0
sub testDefault {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "32K",
       "indexMemorySize"   => "1",
       "logicalSize"       => "1T",
       "physicalSize"      => "300G",
       "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "10674MB",
          'Total BlockMap Pages' => "1292MB"},
       'VDO Usage' => {
         'Usable Size' => "288GB",
         'Dedup Window' => "1024GB"},
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "1MB",
         'Slab Reference Count Usage' => "73MB"},
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##1
sub testPhysical300Logical2TSlabBits20BlockCacheSize32KDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "32K",
       "indexMemorySize"   => "1",
       "logicalSize"       => "2T",
       "physicalSize"      => "300G",
       "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "10674MB",
          'Total BlockMap Pages' => "2585MB"},
       'VDO Usage' => {
         'Usable Size' => "286GB",
         'Dedup Window' => "1024GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "3MB",
         'Slab Reference Count Usage' => "73MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##2
sub testPhysical300Logical3TSlabBits20BlockCacheSize32KDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "32K",
       "indexMemorySize"   => "1",
       "logicalSize"       => "3T",
       "physicalSize"      => "300G",
       "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "10674MB",
          'Total BlockMap Pages' => "3878MB"},
       'VDO Usage' => {
         'Usable Size' => "285GB",
         'Dedup Window' => "1024GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "4MB",
         'Slab Reference Count Usage' => "73MB",
       },
      );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##3
sub testPhysical300Logical4TSlabBits20BlockCacheSize32KSparse {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "32K",
       "indexMemorySize"   => "1",
       "logicalSize"       => "3T",
       "physicalSize"      => "300G",
       "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "91611MB",
          'Total BlockMap Pages' => "3878MB"},
       'VDO Usage' => {
         'Usable Size' => "206GB",
         'Dedup Window' => "1024GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "4MB",
         'Slab Reference Count Usage' => "53MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $cmdArgs .= " --sparseIndex ";
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
## 0
sub testPhysical300Logical1TSlabBits20BlockCacheSize32Ksmall25IndexDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "32K",
       "indexMemorySize"   => "0.25",
       "logicalSize"       => "1T",
       "physicalSize"      => "300G",
       "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "2652MB",
          'Total BlockMap Pages' => "1292MB"},
       'VDO Usage' => {
         'Usable Size' => "295GB",
         'Dedup Window' => "256GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "1MB",
         'Slab Reference Count Usage' => "75MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##1
sub testPhysical300Logical2TSlabBits20BlockCacheSize32KSmall25IndexDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "32K",
       "indexMemorySize"   => "0.25",
       "logicalSize"       => "2T",
       "physicalSize"      => "300G",
       "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "2652MB",
          'Total BlockMap Pages' => "2585MB"},
       'VDO Usage' => {
         'Usable Size' => "294GB",
         'Dedup Window' => "256GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "3MB",
         'Slab Reference Count Usage' => "75MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##2
sub testPhysical300Logical3TSlabBits20BlockCacheSize32KSmall25IndexDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
      "blockMapCacheSize" => "32K",
      "indexMemorySize"   => "0.25",
      "logicalSize"       => "3T",
      "physicalSize"      => "300G",
      "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "2652MB",
          'Total BlockMap Pages' => "3878MB"},
       'VDO Usage' => {
         'Usable Size' => "293GB",
         'Dedup Window' => "256GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "4MB",
         'Slab Reference Count Usage' => "75MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##3
sub testPhysical300Logical4TSlabBits20BlockCacheSize32KSmall25IndexSparse {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
      "blockMapCacheSize" => "32K",
      "indexMemorySize"   => "0.25",
      "logicalSize"       => "3T",
      "physicalSize"      => "300G",
      "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "22743MB",
          'Total BlockMap Pages' => "3878MB"},
       'VDO Usage' => {
         'Usable Size' => "273GB",
         'Dedup Window' => "256GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "4MB",
         'Slab Reference Count Usage' => "70MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $cmdArgs .= " --sparseIndex ";
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
## 0
sub testPhysical300Logical1TSlabBits20BlockCacheSize32Ksmall75IndexDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "32K",
       "indexMemorySize"   => "0.75",
       "logicalSize"       => "1T",
       "physicalSize"      => "300G",
       "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "8022MB",
          'Total BlockMap Pages' => "1292MB"},
       'VDO Usage' => {
         'Usable Size' => "290GB",
         'Dedup Window' => "768GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "1MB",
         'Slab Reference Count Usage' => "74MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##1
sub testPhysical300Logical2TSlabBits20BlockCacheSize32KSmall75IndexDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
      "blockMapCacheSize" => "32K",
      "indexMemorySize"   => "0.75",
      "logicalSize"       => "2T",
      "physicalSize"      => "300G",
      "slabBits"          => "20",
      );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "8022MB",
          'Total BlockMap Pages' => "2585MB"},
       'VDO Usage' => {
         'Usable Size' => "289GB",
         'Dedup Window' => "768GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "3MB",
         'Slab Reference Count Usage' => "74MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##2
sub testPhysical300Logical3TSlabBits20BlockCacheSize32KSmall75IndexDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "32K",
       "indexMemorySize"   => "0.75",
       "logicalSize"       => "3T",
       "physicalSize"      => "300G",
       "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "8022MB",
          'Total BlockMap Pages' => "3878MB"},
       'VDO Usage' => {
         'Usable Size' => "288GB",
         'Dedup Window' => "768GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "4MB",
         'Slab Reference Count Usage' => "74MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##3
sub testPhysical300Logical4TSlabBits20BlockCacheSize32KSmall75IndexSparse {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "32K",
       "indexMemorySize"   => "0.75",
       "logicalSize"       => "3T",
       "physicalSize"      => "300G",
       "slabBits"          => "20",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "1048576" },
       'VDO meta data' => {
          'UDS Index Size' => "68869MB",
          'Total BlockMap Pages' => "3878MB"},
       'VDO Usage' => {
         'Usable Size' => "228GB",
         'Dedup Window' => "768GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "128MB",
         'Forest Memory Usage' => "4MB",
         'Slab Reference Count Usage' => "59MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $cmdArgs .= " --sparseIndex ";
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##
sub testPhysical600Logical1TSlabBits22BlockCacheSize36Klarge1IndexDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "36K",
       "indexMemorySize"   => "1",
       "logicalSize"       => "1T",
       "physicalSize"      => "600G",
       "slabBits"          => "22",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "4194304" },
       'VDO meta data' => {
          'UDS Index Size' => "10674MB",
          'Total BlockMap Pages' => "1292MB"},
       'VDO Usage' => {
         'Usable Size' => "588GB",
         'Dedup Window' => "1024GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "144MB",
         'Forest Memory Usage' => "1MB",
         'Slab Reference Count Usage' => "149MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##1
sub testPhysical600Logical2TSlabBits22BlockCacheSize36Klarge1IndexDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
       "blockMapCacheSize" => "36K",
       "indexMemorySize"   => "1",
       "logicalSize"       => "2T",
       "physicalSize"      => "600G",
       "slabBits"          => "22",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "4194304" },
       'VDO meta data' => {
          'UDS Index Size' => "10674MB",
          'Total BlockMap Pages' => "2585MB"},
       'VDO Usage' => {
         'Usable Size' => "586GB",
         'Dedup Window' => "1024GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "144MB",
         'Forest Memory Usage' => "3MB",
         'Slab Reference Count Usage' => "149MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

######################################################################
##
sub testPhysical600Logical3TSlabBits22BlockCacheSize36Klarge1IndexDense {
  my ($self) = assertNumArgs(1, @_);
  my $device = $self->getDevice();
  my $machine = $device->getMachine();
  my $cmd = $self->findBinary("vdoCalculateSize");
  my %vdoCalculateSizeArgs = (
      "blockMapCacheSize" => "36K",
      "indexMemorySize"   => "1",
      "logicalSize"       => "3T",
      "physicalSize"      => "600G",
      "slabBits"          => "22",
     );
  my %expectedResult = (
       'VDO' => { 'Slab Size' => "4194304" },
       'VDO meta data' => {
          'UDS Index Size' => "10674MB",
          'Total BlockMap Pages' => "3878MB"},
       'VDO Usage' => {
         'Usable Size' => "585GB",
         'Dedup Window' => "1024GB",
       },
       'In Memory Usage' => {
         'Block Map Cache'  => "144MB",
         'Forest Memory Usage' => "4MB",
         'Slab Reference Count Usage' => "149MB",
       },
     );
  my $cmdArgs = "";
  foreach my $arg (keys %vdoCalculateSizeArgs) {
    $cmdArgs .= "--" . "$arg" . " $vdoCalculateSizeArgs{$arg} ";
  }
  $log->info("CMD ARGS $cmdArgs\n");
  $machine->runSystemCmd("$cmd $cmdArgs");
  my $sizeYaml = YAML::Load($machine->getStdout());
  $log->info("cmd: $cmd $cmdArgs\n");
  foreach my $key1 (keys %expectedResult) {
    foreach my $key2 (keys %{$expectedResult{$key1} }) {
      $log->info("Expected: $expectedResult{$key1}{$key2}\n");
      $log->info("Cmd: $sizeYaml->{$key1}->{$key2}\n");
      assertEq($expectedResult{$key1}{$key2}, $sizeYaml->{$key1}->{$key2});
    }
  }
}

1;
