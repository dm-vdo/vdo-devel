#############################################################################
# Mock Permabit::MegaRaid::MegaCli class, for testing
#
# $Id$
##
package Permabit::MegaRaid::MockMegaRaid::MegaCli;

use strict;
use warnings FATAL => qw(all);
use Carp qw(croak);
use English qw(-no_match_vars);

use Log::Log4perl;
use Permabit::Assertions qw(
  assertNumDefinedArgs
  assertTrue
);

use base qw(Permabit::MegaRaid::MegaCli);

# TODO, for more robust testing:
#  * Mock output should have multiple disks per device instead of just one
#  * Mock output should have physical disks that have no virtual device
#     membership.

# Log4perl logging object
my $log = Log::Log4perl->get_logger(__PACKAGE__);

# -L must come before -A
my $LD_BEFORE_ADAPTER_RULE = qr/-L\d(?!.*-A\d)/;

#############################################################################
# @inherit
##
sub run {
  my ($self) = assertNumDefinedArgs(1, @_);

  # Using MegaRaid::MegaCli::getArguments() and parsing the resulting
  #  @args strings asserts that we're not testing something that it can
  #  not handle.
  my @args = $self->SUPER::getArguments();
  my $str  = join(" ", @args);

  # check LD_BEFORE_ADAPTER_RULE
  assertTrue(($str =~ $LD_BEFORE_ADAPTER_RULE),
             "-L<logical id> must come before -A<adapter id>");

  # XXX we naively assume that the rest of the arguments make sense
  my %argMap = (
    "-AdpAllInfo"   => \&getAdapterInfoOut,
    "-LDPDInfo"     => \&getLdPdInfoOut,
    "-EncInfo"      => \&getEnclosureInfoOut,
    "-PDList"       => \&getPdInfoOut,
    "-CfgLDDel"     => \&returnBlankStdout,
    "-CfgLDAdd"     => \&returnBlankStdout,
  );

  if ($args[0] =~ qr(-LDInit)) {
    return returnBlankStdout();
  } elsif (defined($argMap{$args[0]})) {
    return $argMap{$args[0]}->();
  } else {
    croak("Unrecognized first argument to MegaCli: $args[0]");
  }
}

#############################################################################
# Return a blank out string.
##
sub returnBlankStdout {
  return { stdout => "" };
}

#############################################################################
# Mock adapter info out string
#
#  * adapter id 0
#  * 4 drives total
#  * 2 configured volumes
#
#  @return the string
##
sub getAdapterInfoOut {
  return { stdout =>
"

Adapter #0

==============================================================================
                    Versions
                ================
Product Name    : LSI MegaRAID SAS 9240-4i
Serial No       : SP12112600
FW Package Build: 20.10.1-0061

                    Mfg. Data
                ================
Mfg. Date       : 05/25/11
Rework Date     : 00/00/00
Revision No     : 03A
Battery FRU     : N/A

                Image Versions in Flash:
                ================
BIOS Version       : 4.29.00_4.12.05.00_0x05080000
Preboot CLI Version: 03.02-015:#%00008
WebBIOS Version    : 4.0-54-e_42-Rel
NVDATA Version     : 3.09.03-0027
FW Version         : 2.120.184-1415
Boot Block Version : 2.02.00.00-0001

                Pending Images in Flash
                ================
None

                PCI Info
                ================
Vendor Id       : 1000
Device Id       : 0073
SubVendorId     : 1000
SubDeviceId     : 9241

Host Interface  : PCIE

ChipRevision    : B2

Number of Frontend Port: 0
Device Interface  : PCIE

Number of Backend Port: 8
Port  :  Address
0        4433221101000000
1        4433221103000000
2        0000000000000000
3        0000000000000000
4        0000000000000000
5        0000000000000000
6        0000000000000000
7        0000000000000000

                HW Configuration
                ================
SAS Address      : 500605b003cc8660
BBU              : Absent
Alarm            : Absent
NVRAM            : Present
Serial Debugger  : Present
Memory           : Absent
Flash            : Present
Memory Size      : 0MB
TPM              : Absent
On board Expander: Absent
Upgrade Key      : Absent
Temperature sensor for ROC    : Absent
Temperature sensor for controller    : Absent


                Settings
                ================
Current Time                     : 15:7:25 3/15, 2012
Predictive Fail Poll Interval    : 300sec
Interrupt Throttle Active Count  : 16
Interrupt Throttle Completion    : 50us
Rebuild Rate                     : 30%
PR Rate                          : 30%
BGI Rate                         : 30%
Check Consistency Rate           : 30%
Reconstruction Rate              : 30%
Cache Flush Interval             : 4s
Max Drives to Spinup at One Time : 4
Delay Among Spinup Groups        : 2s
Physical Drive Coercion Mode     : 1GB
Cluster Mode                     : Disabled
Alarm                            : Disabled
Auto Rebuild                     : Enabled
Battery Warning                  : Disabled
Ecc Bucket Size                  : 15
Ecc Bucket Leak Rate             : 1440 Minutes
Restore HotSpare on Insertion    : Disabled
Expose Enclosure Devices         : Enabled
Maintain PD Fail History         : Enabled
Host Request Reordering          : Enabled
Auto Detect BackPlane Enabled    : SGPIO/i2c SEP
Load Balance Mode                : Auto
Use FDE Only                     : No
Security Key Assigned            : No
Security Key Failed              : No
Security Key Not Backedup        : No
Default LD PowerSave Policy      : Controller Defined
Maximum number of direct attached drives to spin up in 1 min : 0
Any Offline VD Cache Preserved   : No
Allow Boot with Preserved Cache  : No
Disable Online Controller Reset  : No
PFK in NVRAM                     : No
Use disk activity for locate     : No

                Capabilities
                ================
RAID Level Supported             : RAID0, RAID1, RAID5, RAID00, RAID10, RAID50, PRL 11, PRL 11 with spanning, SRL 3 supported, PRL11-RLQ0 DDF layout with no span, PRL11-RLQ0 DDF layout with span
Supported Drives                 : SAS, SATA

Allowed Mixing:

Mix in Enclosure Allowed
Mix of SAS/SATA of HDD type in VD Allowed

                Status
                ================
ECC Bucket Count                 : 0

                Limitations
                ================
Max Arms Per VD          : 16
Max Spans Per VD         : 8
Max Arrays               : 16
Max Number of VDs        : 16
Max Parallel Commands    : 31
Max SGE Count            : 80
Max Data Transfer Size   : 8192 sectors
Max Strips PerIO         : 20
Max LD per array         : 16
Min Strip Size           : 8 KB
Max Strip Size           : 64 KB
Max Configurable CacheCade Size: 0 GB
Current Size of CacheCade      : 0 GB
Current Size of FW Cache       : 0 MB

                Device Present
                ================
Virtual Drives    : 2
  Degraded        : 0
  Offline         : 0
Physical Devices  : 5
  Disks           : 4
  Critical Disks  : 0
  Failed Disks    : 0

                Supported Adapter Operations
                ================
Rebuild Rate                    : Yes
CC Rate                         : Yes
BGI Rate                        : Yes
Reconstruct Rate                : Yes
Patrol Read Rate                : Yes
Alarm Control                   : Yes
Cluster Support                 : No
BBU                             : No
Spanning                        : Yes
Dedicated Hot Spare             : Yes
Revertible Hot Spares           : Yes
Foreign Config Import           : Yes
Self Diagnostic                 : Yes
Allow Mixed Redundancy on Array : No
Global Hot Spares               : Yes
Deny SCSI Passthrough           : No
Deny SMP Passthrough            : No
Deny STP Passthrough            : No
Support Security                : No
Snapshot Enabled                : No
Support the OCE without adding drives : Yes
Support PFK                     : Yes
Support PI                      : No
Support Boot Time PFK Change    : Yes
Disable Online PFK Change       : No
PFK TrailTime Remaining         : 0 days 0 hours
Support Shield State            : No
Block SSD Write Disk Cache Change: No

                Supported VD Operations
                ================
Read Policy          : No
Write Policy         : No
IO Policy            : No
Access Policy        : Yes
Disk Cache Policy    : Yes
Reconstruction       : Yes
Deny Locate          : No
Deny CC              : No
Allow Ctrl Encryption: No
Enable LDBBM         : No
Support Breakmirror  : No
Power Savings        : No

                Supported PD Operations
                ================
Force Online                            : Yes
Force Offline                           : Yes
Force Rebuild                           : Yes
Deny Force Failed                       : No
Deny Force Good/Bad                     : No
Deny Missing Replace                    : No
Deny Clear                              : No
Deny Locate                             : No
Support Temperature                     : Yes
Disable Copyback                        : No
Enable JBOD                             : Yes
Enable Copyback on SMART                : No
Enable Copyback to SSD on SMART Error   : Yes
Enable SSD Patrol Read                  : No
PR Correct Unconfigured Areas           : Yes
Enable Spin Down of UnConfigured Drives : Yes
Disable Spin Down of hot spares         : Yes
Spin Down time                          : 30
T10 Power State                         : No
                Error Counters
                ================
Memory Correctable Errors   : 0
Memory Uncorrectable Errors : 0

                Cluster Information
                ================
Cluster Permitted     : No
Cluster Active        : No

                Default Settings
                ================
Phy Polarity                     : 0
Phy PolaritySplit                : 0
Background Rate                  : 30
Strip Size                       : 64kB
Flush Time                       : 4 seconds
Write Policy                     : WT
Read Policy                      : None
Cache When BBU Bad               : Disabled
Cached IO                        : No
SMART Mode                       : Mode 6
Alarm Disable                    : No
Coercion Mode                    : None
ZCR Config                       : Unknown
Dirty LED Shows Drive Activity   : No
BIOS Continue on Error           : No
Spin Down Mode                   : None
Allowed Device Type              : SAS/SATA Mix
Allow Mix in Enclosure           : Yes
Allow HDD SAS/SATA Mix in VD     : Yes
Allow SSD SAS/SATA Mix in VD     : No
Allow HDD/SSD Mix in VD          : No
Allow SATA in Cluster            : No
Max Chained Enclosures           : 2
Disable Ctrl-R                   : Yes
Enable Web BIOS                  : Yes
Direct PD Mapping                : No
BIOS Enumerate VDs               : Yes
Restore Hot Spare on Insertion   : No
Expose Enclosure Devices         : Yes
Maintain PD Fail History         : Yes
Disable Puncturing               : Yes
Zero Based Enclosure Enumeration : No
PreBoot CLI Enabled              : Yes
LED Show Drive Activity          : No
Cluster Disable                  : Yes
SAS Disable                      : No
Auto Detect BackPlane Enable     : SGPIO
Use FDE Only                     : No
Enable Led Header                : No
Delay during POST                : 0
EnableCrashDump                  : No
Disable Online Controller Reset  : No
EnableLDBBM                      : No
Un-Certified Hard Disk Drives    : Allow
Treat Single span R1E as R10     : No
Max LD per array                 : 16
Power Saving option              : All power saving options are disabled
Default spin down time in minutes: 30
Enable JBOD                      : Yes
TTY Log In Flash                 : Yes
Auto Enhanced Import             : No
BreakMirror RAID Support         : No
Disable Join Mirror              : No
Enable Shield State              : No
Time taken to detect CME         : 60s

Exit Code: 0x00"};
}

#############################################################################
# Mock physical disk info out string
#
#  * four disks
#  * Slot numbers 0, 1, 2, and 3
#  * Device IDs 7, 5, 9, 4
#
#  @return the string
##
sub getPdInfoOut {
  return { stdout =>
"

Adapter #0

Enclosure Device ID: 64
Slot Number: 0
Drive's postion: DiskGroup: 0, Span: 0, Arm: 0
Enclosure position: 0
Device Id: 7
WWN:
Sequence Number: 8
Media Error Count: 0
Other Error Count: 0
Predictive Failure Count: 0
Last Predictive Failure Event Seq Number: 0
PD Type: SATA
Raw Size: 232.885 GB [0x1d1c5970 Sectors]
Non Coerced Size: 232.385 GB [0x1d0c5970 Sectors]
Coerced Size: 231.898 GB [0x1cfcc000 Sectors]
Firmware state: Online, Spun Up
Is Commissioned Spare : NO
Device Firmware Level: PWG4
Shield Counter: 0
Successful diagnostics completion on :  N/A
SAS Address(0): 0x4433221103000000
Connected Port Number: 1(path0)
Inquiry Data: LNEL123300AX250DGN  INTEL SSDSC2MH250A2                     PWG4
FDE Enable: Disable
Secured: Unsecured
Locked: Unlocked
Needs EKM Attention: No
Foreign State: None
Device Speed: 6.0Gb/s
Link Speed: 6.0Gb/s
Media Type: Solid State Device
Drive Temperature :65C (149.00 F)
PI Eligibility:  No
Drive is formatted for PI information:  No
PI: No PI
Drive's write cache : Disabled
Drive's NCQ setting : Disabled
Port-0 :
Port status: Active
Port's Linkspeed: 6.0Gb/s
Drive has flagged a S.M.A.R.T alert : No




Enclosure Device ID: 64
Slot Number: 1
Drive's postion: DiskGroup: 0, Span: 0, Arm: 0
Enclosure position: 0
Device Id: 5
WWN:
Sequence Number: 8
Media Error Count: 0
Other Error Count: 0
Predictive Failure Count: 0
Last Predictive Failure Event Seq Number: 0
PD Type: SATA
Raw Size: 232.885 GB [0x1d1c5970 Sectors]
Non Coerced Size: 232.385 GB [0x1d0c5970 Sectors]
Coerced Size: 231.898 GB [0x1cfcc000 Sectors]
Firmware state: Online, Spun Up
Is Commissioned Spare : NO
Device Firmware Level: PWG4
Shield Counter: 0
Successful diagnostics completion on :  N/A
SAS Address(0): 0x4433221103000000
Connected Port Number: 1(path0)
Inquiry Data: LNEL123300AX250DGN  INTEL SSDSC2MH250A2                     PWG4
FDE Enable: Disable
Secured: Unsecured
Locked: Unlocked
Needs EKM Attention: No
Foreign State: None
Device Speed: 6.0Gb/s
Link Speed: 6.0Gb/s
Media Type: Solid State Device
Drive Temperature :65C (149.00 F)
PI Eligibility:  No
Drive is formatted for PI information:  No
PI: No PI
Drive's write cache : Disabled
Drive's NCQ setting : Disabled
Port-0 :
Port status: Active
Port's Linkspeed: 6.0Gb/s
Drive has flagged a S.M.A.R.T alert : No




Enclosure Device ID: 64
Slot Number: 2
Drive's postion: DiskGroup: 0, Span: 0, Arm: 1
Enclosure position: 0
Device Id: 9
WWN:
Sequence Number: 8
Media Error Count: 0
Other Error Count: 0
Predictive Failure Count: 0
Last Predictive Failure Event Seq Number: 0
PD Type: SATA
Raw Size: 232.885 GB [0x1d1c5970 Sectors]
Non Coerced Size: 232.385 GB [0x1d0c5970 Sectors]
Coerced Size: 231.898 GB [0x1cfcc000 Sectors]
Firmware state: Online, Spun Up
Is Commissioned Spare : NO
Device Firmware Level: PWG4
Shield Counter: 0
Successful diagnostics completion on :  N/A
SAS Address(0): 0x4433221103000000
Connected Port Number: 1(path0)
Inquiry Data: LNEL123300AX250DGN  INTEL SSDSC2MH250A2                     PWG4
FDE Enable: Disable
Secured: Unsecured
Locked: Unlocked
Needs EKM Attention: No
Foreign State: None
Device Speed: 6.0Gb/s
Link Speed: 6.0Gb/s
Media Type: Solid State Device
Drive Temperature :65C (149.00 F)
PI Eligibility:  No
Drive is formatted for PI information:  No
PI: No PI
Drive's write cache : Disabled
Drive's NCQ setting : Disabled
Port-0 :
Port status: Active
Port's Linkspeed: 6.0Gb/s
Drive has flagged a S.M.A.R.T alert : No




Enclosure Device ID: 64
Slot Number: 3
Drive's postion: DiskGroup: 0, Span: 0, Arm: 1
Enclosure position: 0
Device Id: 4
WWN:
Sequence Number: 8
Media Error Count: 0
Other Error Count: 0
Predictive Failure Count: 0
Last Predictive Failure Event Seq Number: 0
PD Type: SATA
Raw Size: 232.885 GB [0x1d1c5970 Sectors]
Non Coerced Size: 232.385 GB [0x1d0c5970 Sectors]
Coerced Size: 231.898 GB [0x1cfcc000 Sectors]
Firmware state: Online, Spun Up
Is Commissioned Spare : NO
Device Firmware Level: PWG4
Shield Counter: 0
Successful diagnostics completion on :  N/A
SAS Address(0): 0x4433221101000000
Connected Port Number: 0(path0)
Inquiry Data: LNEL123300YQ250DGN  INTEL SSDSC2MH250A2                     PWG4
FDE Enable: Disable
Secured: Unsecured
Locked: Unlocked
Needs EKM Attention: No
Foreign State: None
Device Speed: 6.0Gb/s
Link Speed: 6.0Gb/s
Media Type: Solid State Device
Drive Temperature :65C (149.00 F)
PI Eligibility:  No
Drive is formatted for PI information:  No
PI: No PI
Drive's write cache : Disabled
Drive's NCQ setting : Disabled
Port-0 :
Port status: Active
Port's Linkspeed: 6.0Gb/s
Drive has flagged a S.M.A.R.T alert : No




Exit Code: 0x00"};
}

#############################################################################
# Mock physical disk, virtual disk info out string
#
#  * in accordance with $adapterInfoOut
#
#  Disk info:
#
#  * four disks
#  * Disk Slot numbers 0 through 3, Disk Device IDs 7, 5, 9, 4
#
#  Virtual device info:
#  * 2 virtual devices
#  * 2 drive in each virtual device
#  * write cache = WriteThrough
#  * read cache  = ReadAheadNone
#  * lsi cache   = Direct
#  * disk cache  = Enabled
#  * stripe      = 64 KB
#  * raid level  = 0
#
#  @return the string
##
sub getLdPdInfoOut {
  return { stdout =>
"

Adapter #0

Number of Virtual Disks: 2
Virtual Drive: 0 (Target Id: 0)
Name                :
RAID Level          : Primary-0, Secondary-0, RAID Level Qualifier-0
Size                : 462.123 GB
Parity Size         : 0
State               : Optimal
Strip Size          : 64 KB
Number Of Drives    : 2
Span Depth          : 1
Default Cache Policy: WriteThrough, ReadAheadNone, Direct, No Write Cache if Bad BBU
Current Cache Policy: WriteThrough, ReadAheadNone, Direct, No Write Cache if Bad BBU
Default Access Policy: Read/Write
Current Access Policy: Read/Write
Disk Cache Policy   : Disk's Default
Encryption Type     : None
Is VD Cached: No
Number of Spans: 1
Span: 0 - Number of PDs: 2


PD: 0 Information
Enclosure Device ID: 64
Slot Number: 0
Drive's postion: DiskGroup: 0, Span: 0, Arm: 0
Enclosure position: 0
Device Id: 7
WWN:
Sequence Number: 10
Media Error Count: 0
Other Error Count: 0
Predictive Failure Count: 0
Last Predictive Failure Event Seq Number: 0
PD Type: SATA
Raw Size: 232.885 GB [0x1d1c5970 Sectors]
Non Coerced Size: 232.385 GB [0x1d0c5970 Sectors]
Coerced Size: 231.898 GB [0x1cfcc000 Sectors]
Firmware state: Online, Spun Up
Is Commissioned Spare : NO
Device Firmware Level: PWG4
Shield Counter: 0
Successful diagnostics completion on :  N/A
SAS Address(0): 0x4433221101000000
Connected Port Number: 0(path0)
Inquiry Data: LNEL123300YQ250DGN  INTEL SSDSC2MH250A2                     PWG4
FDE Enable: Disable
Secured: Unsecured
Locked: Unlocked
Needs EKM Attention: No
Foreign State: None
Device Speed: 6.0Gb/s
Link Speed: 6.0Gb/s
Media Type: Solid State Device
Drive Temperature :65C (149.00 F)
PI Eligibility:  No
Drive is formatted for PI information:  No
PI: No PI
Drive's write cache : Disabled
Drive's NCQ setting : Disabled
Port-0 :
Port status: Active
Port's Linkspeed: 6.0Gb/s
Drive has flagged a S.M.A.R.T alert : No



PD: 1 Information
Enclosure Device ID: 64
Slot Number: 1
Drive's postion: DiskGroup: 0, Span: 0, Arm: 0
Enclosure position: 0
Device Id: 5
WWN:
Sequence Number: 10
Media Error Count: 0
Other Error Count: 0
Predictive Failure Count: 0
Last Predictive Failure Event Seq Number: 0
PD Type: SATA
Raw Size: 232.885 GB [0x1d1c5970 Sectors]
Non Coerced Size: 232.385 GB [0x1d0c5970 Sectors]
Coerced Size: 231.898 GB [0x1cfcc000 Sectors]
Firmware state: Online, Spun Up
Is Commissioned Spare : NO
Device Firmware Level: PWG4
Shield Counter: 0
Successful diagnostics completion on :  N/A
SAS Address(0): 0x4433221101000000
Connected Port Number: 0(path0)
Inquiry Data: LNEL123300YQ250DGN  INTEL SSDSC2MH250A2                     PWG4
FDE Enable: Disable
Secured: Unsecured
Locked: Unlocked
Needs EKM Attention: No
Foreign State: None
Device Speed: 6.0Gb/s
Link Speed: 6.0Gb/s
Media Type: Solid State Device
Drive Temperature :65C (149.00 F)
PI Eligibility:  No
Drive is formatted for PI information:  No
PI: No PI
Drive's write cache : Disabled
Drive's NCQ setting : Disabled
Port-0 :
Port status: Active
Port's Linkspeed: 6.0Gb/s
Drive has flagged a S.M.A.R.T alert : No




Virtual Drive: 1 (Target Id: 1)
Name                :
RAID Level          : Primary-0, Secondary-0, RAID Level Qualifier-0
Size                : 462.123 GB
Parity Size         : 0
State               : Optimal
Strip Size          : 64 KB
Number Of Drives    : 2
Span Depth          : 1
Default Cache Policy: WriteThrough, ReadAheadNone, Direct, No Write Cache if Bad BBU
Current Cache Policy: WriteThrough, ReadAheadNone, Direct, No Write Cache if Bad BBU
Default Access Policy: Read/Write
Current Access Policy: Read/Write
Disk Cache Policy   : Disk's Default
Encryption Type     : None
Is VD Cached: No
Number of Spans: 1
Span: 0 - Number of PDs: 2

PD: 0 Information
Enclosure Device ID: 64
Slot Number: 2
Drive's postion: DiskGroup: 1, Span: 0, Arm: 0
Enclosure position: 0
Device Id: 9
WWN:
Sequence Number: 10
Media Error Count: 0
Other Error Count: 0
Predictive Failure Count: 0
Last Predictive Failure Event Seq Number: 0
PD Type: SATA
Raw Size: 232.885 GB [0x1d1c5970 Sectors]
Non Coerced Size: 232.385 GB [0x1d0c5970 Sectors]
Coerced Size: 231.898 GB [0x1cfcc000 Sectors]
Firmware state: Online, Spun Up
Is Commissioned Spare : NO
Device Firmware Level: PWG4
Shield Counter: 0
Successful diagnostics completion on :  N/A
SAS Address(0): 0x4433221103000000
Connected Port Number: 1(path0)
Inquiry Data: LNEL123300AX250DGN  INTEL SSDSC2MH250A2                     PWG4
FDE Enable: Disable
Secured: Unsecured
Locked: Unlocked
Needs EKM Attention: No
Foreign State: None
Device Speed: 6.0Gb/s
Link Speed: 6.0Gb/s
Media Type: Solid State Device
Drive Temperature :65C (149.00 F)
PI Eligibility:  No
Drive is formatted for PI information:  No
PI: No PI
Drive's write cache : Disabled
Drive's NCQ setting : Disabled
Port-0 :
Port status: Active
Port's Linkspeed: 6.0Gb/s
Drive has flagged a S.M.A.R.T alert : No

PD: 1 Information
Enclosure Device ID: 64
Slot Number: 3
Drive's postion: DiskGroup: 1, Span: 0, Arm: 0
Enclosure position: 0
Device Id: 4
WWN:
Sequence Number: 10
Media Error Count: 0
Other Error Count: 0
Predictive Failure Count: 0
Last Predictive Failure Event Seq Number: 0
PD Type: SATA
Raw Size: 232.885 GB [0x1d1c5970 Sectors]
Non Coerced Size: 232.385 GB [0x1d0c5970 Sectors]
Coerced Size: 231.898 GB [0x1cfcc000 Sectors]
Firmware state: Online, Spun Up
Is Commissioned Spare : NO
Device Firmware Level: PWG4
Shield Counter: 0
Successful diagnostics completion on :  N/A
SAS Address(0): 0x4433221103000000
Connected Port Number: 1(path0)
Inquiry Data: LNEL123300AX250DGN  INTEL SSDSC2MH250A2                     PWG4
FDE Enable: Disable
Secured: Unsecured
Locked: Unlocked
Needs EKM Attention: No
Foreign State: None
Device Speed: 6.0Gb/s
Link Speed: 6.0Gb/s
Media Type: Solid State Device
Drive Temperature :65C (149.00 F)
PI Eligibility:  No
Drive is formatted for PI information:  No
PI: No PI
Drive's write cache : Disabled
Drive's NCQ setting : Disabled
Port-0 :
Port status: Active
Port's Linkspeed: 6.0Gb/s
Drive has flagged a S.M.A.R.T alert : No




Exit Code: 0x00"};
}

#############################################################################
# Mock enclosure info out string
#
#  * Device ID: 64
#
#  @return the string
##
sub getEnclosureInfoOut {
  return { stdout =>
"

    Number of enclosures on adapter 0 -- 1

    Enclosure 0:
    Device ID                     : 64
    Number of Slots               : 8
    Number of Power Supplies      : 0
    Number of Fans                : 0
    Number of Temperature Sensors : 0
    Number of Alarms              : 0
    Number of SIM Modules         : 1
    Number of Physical Drives     : 4
    Status                        : Normal
    Position                      : 1
    Connector Name                : Unavailable
    Enclosure type                : SGPIO
Failed in first Inquiry commnad
    FRU Part Number               : N/A
    Enclosure Serial Number       : N/A
    ESM Serial Number             : N/A
    Enclosure Zoning Mode         : N/A
    Partner Device Id             : Unavailable

    Inquiry data                  :
        Vendor Identification     : LSI
        Product Identification    : SGPIO
        Product Revision Level    : N/A
        Vendor Specific           :


Exit Code: 0x00"};
}

1;
