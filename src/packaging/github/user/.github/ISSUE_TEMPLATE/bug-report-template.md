---
name: Bug Report Template
about: Issue Template for a Bug Report
title: "[BUG]"
labels: bug
assignees: rhawalsh

---

**Describe the bug:**
A clear and concise description of what the bug is.

**To Reproduce:**
Steps to reproduce the behavior:
1. 
2. 
3. 

**VDO Configuration:**
 - VDO Create Command Used: [e.g. `vdo create --name vdo0 --device /dev/sda --vdoLogicalSize 10T --indexMem 1`]
 - Contents of /etc/vdoconf.yml
 - Output of vdodumpconfig: [e.g. `vdodumpconfig /dev/sda`]
 - Output of `vdo status`:

**Expected behavior:**
A clear and concise description of what you expected to happen.

**Storage Stack Configuration:**
High Level Configuration: [e.g. SSD -> MD RAID 5 -> VDO -> XFS]
Output of `blockdev --report`: 
Output of `dmsetup ls --tree`:
Output of `lsblk -o name,maj:min,kname,type,fstype,state,sched,uuid`:

**Hardware Information:**
 - CPU: [e.g. 2x Intel Xeon E5-1650 v2 @ 3.5GHz]
 - Memory: [e.g. 128G]
 - Storage: [e.g. Intel Optane SSD 900P]
 - Other: [e.g. iSCSI backed storage]

**Distro Information:**
 - OS: [e.g. RHEL-7.5]
 - Architecture: [e.g. x86_64]
 - Kernel: [e.g. kernel-3.10.0-862.el7]
 - VDO Version: [e.g. vdo-6.2.0.168-18.el7, or a commit hash]
 - KVDO Version: [e.g. kmod-kvdo6.2.0.153-15.el7, or a commit hash]
 - LVM Version: [e.g. 2.02.177-4.el7]
 - Output of `uname -a`: [e.g. Linux localhost.localdomain 3.10.0-862.el7.x86_64 #1 SMP Wed Mar 21 18:14:51 EDT 2018 x86_64 x86_64 x86_64 GNU/Linux]

**Additional context:**
Add any other context about the problem here.
