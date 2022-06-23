#!/bin/bash
#
# collect-vdodiagnostics - VDO Diagnostics Collection
#
# %COPYRIGHT%
#
# %LICENSE%
# 
# $Id$
#
# This script will collect all relevant log files and system-state information
# to be used by support for diagnosing operations.
#

# Error Check and abort function after tar
checkTarErrStatus() {
  collectionDir=$1  
  errCode=$2
  errLevel=$3

  if [ $errCode -gt $errLevel ]
  then
    printf "\nError: tar on $collectionDir returned with ErrCode $errCode\n"
    sudo rm -rf $collectionDir
    exit 1
  fi
}

# Create the area to place files while collecting
buildCollectionArea() {
  printf "Building collection environment ... "
  collectionDir=$1

  sudo mkdir -p -m 777 $collectionDir

  printf "done.\n"
}

# Perform the SOSReport
sosReport() {
  printf "Collecting sosreport ... "
  collectionDir=$1

  sudo sosreport --batch --tmp-dir $collectionDir > /dev/null 2>&1

  printf "done.\n"
}

# Archive relevant system files
sysArchive() {
  printf "Collecting system files ... "
  collectionDir=$1

  sudo tar --warning=none \
           --exclude='/etc/selinux/targeted*' \
           -czPf \
           $collectionDir/sysArchive.tar.gz \
           /var/log /var/run /var/cache /var/lib/dkms /etc
  errcode=$(echo $?)

  checkTarErrStatus $collectionDir $errcode 1
  #exit 1 here means some files were changed while reading e.g. syslog
  printf "done.\n"
}

# Collect system-specific data
function gatherSysData() {
  printf "Collecting system data ... "
  collectionDir=$1

  cat /proc/meminfo > $collectionDir/meminfo.out 2>&1
  sudo chkconfig > $collectionDir/chkconfig.out 2>&1
  df -lh > $collectionDir/df-lh.out 2>&1
  df -l > $collectionDir/df-l.out 2>&1
  free > $collectionDir/free.out 2>&1
  sudo lsof > $collectionDir/lsof.out 2>&1
  sudo lsscsi -g > $collectionDir/lsscsi.out 2>&1
  top -b -n1 > $collectionDir/top.out 2>&1

  #Gather more data from ipmi lan and dmi decode
  sudo ipmitool lan print > $collectionDir/ipmitool.out 2>&1
  sudo ipmitool sel elist > $collectionDir/ipmiEvents.out 2>&1
  sudo dmidecode > $collectionDir/idmidecode.log 2>&1
  
  printf "done.\n"
}

# Collect VDO data
function gatherVdoData() {
  printf "Collecting VDO data ... "
  collectionDir=$1

  cp /etc/vdoconf.xml $collectionDir/ 2> /dev/null
  modinfo kvdo > $collectionDir/modinfo-kvdo.out 2>&1
  sudo vdoStats --verbose > $collectionDir/vdoStats.out 2>&1
  sudo vdoStats --queues > $collectionDir/vdoStatsQs.out 2>&1
  
  printf "done.\n"
}

# Collect MD RAID data
function gatherMdData() {
  printf "Collecting MD RAID data ... "
  collectionDir=$1

  sudo cp /etc/mdadm.conf $collectionDir/mdadm.conf 2> /dev/null
  sudo cat /proc/mdstat > $collectionDir/mdstat.out 2> /dev/null

  printf "done.\n"
}

# Collect multipath data
function gatherMultipathData() {
  printf "Collecting multipath data ... "
  collectionDir=$1

  sudo multipath -ll > $collectionDir/mpll.out 2>&1
  sudo cp /etc/multipath.conf $collectionDir/multipath.conf 2> /dev/null
  sudo multipathd -k"show paths" > $collectionDir/multipathd.out 2>&1
  sudo multipathd -k"show maps status" >> $collectionDir/multipathd.out 2>&1
  sudo multipathd -k"show maps stats" >> $collectionDir/multipathd.out 2>&1

  printf "done.\n"
}

# Collect Device-Mapper data
function gatherDmData() {
  printf "Collecting device-mapper data ... "
  collectionDir=$1

  sudo dmsetup status > $collectionDir/dmsetup.out 2>&1
  sudo dmsetup info -c >> $collectionDir/dmsetup.out 2>&1

  printf "done.\n"
}

# Collect service-related data
function serviceOutput() {
  collectionDir=$1

  gatherDmData $collectionDir
  gatherMultipathData $collectionDir
  gatherMdData $collectionDir
  gatherVdoData $collectionDir
  gatherSysData $collectionDir
}

# Generate the md5sums on all of the files to ensure integrity
function generateMD5() {
  printf "Generating checksums ... "
  collectionDir=$1

  sudo find $collectionDir -type f -exec md5sum "{}" \; > /tmp/checklist.chk
  sudo mv /tmp/checklist.chk $collectionDir

  printf "done.\n"
}

# Generate the tarball for transport
function generateTar() {
  printf "Generating VDO Diagnostics tarball ... "
  collectionDir=$1
  tarFile=$2

  sudo tar --warning=none -czPf $tarFile $collectionDir
  errcode=$(echo $?)
  sudo rm -rf $collectionDir

  checkTarErrStatus $collectionDir $errcode 0
  printf "done.\n"
}

function checkDiskSpace() {
  printf "Checking disk space ... "
  collectionDir=$1
  varSize=$(sudo du -sb /var/ | awk '{print $1}')
  etcSize=$(sudo du -sb /etc/ | awk '{print $1}')
  collectionSize=$((varSize + etcSize))
  printf "Expected collection size: $collectionSize bytes.\n"
}

function startCollection() {
  data=$1
  dateStamp=$(date +%Y%m%d-%H%M%S)
  collection=${data}${dateStamp}
  serialNo=$(sudo dmidecode -s system-serial-number)
  outputFile=${data}vdo-diag-${HOSTNAME}-${dateStamp}.tar.gz

  checkDiskSpace $data
  buildCollectionArea $collection
  sosReport $collection
  sysArchive $collection
  serviceOutput $collection
  generateMD5 $collection
  generateTar $collection $outputFile

  printf "\nVDO Diagnostics collected.\n"
  printf "  Filename: %s\n" "$outputFile"
  printf "  Size:     %s\n" "$(du -h "$outputFile" | awk '{print $1}')"
  printf "  MD5:      %s\n" "$(md5sum $outputFile | awk '{print $1}')"
}

inputpath=$1
if [ -z $inputpath ]; then
  printf "Please enter a destination path with a trailing slash.\n"
  exit 1
fi
if [[ "$inputpath" != */ ]]; then
  printf "Please enter a destination path with a trailing slash.\n"
  exit 1
fi
startCollection $inputpath

exit 0
