#! /bin/bash

##
# Automatic Backup for nightly logfiles from u1 to nbu1 and from nbu1 to dogfood
#
# @ Description
#
# This script checks the nightly-build directory and see if there are any tests which are older 
# than two days (really 3 as it's 24-hour blocks), then archives those off to a folder on nbu1. 
# It then checks nbu1 in that folder and archives to dogfood a tar.gz of anything older than 6 days.
#
# The intent is that this script runs as a cron job at approximately 16:00 each day to make room
# for the new night's run. If run too early it may leave an extra day of data on the drive. This
# might slow down the start of the run if the logs being backed up are huge and require a lot of
# CPU time. The tar creation and copy are both slow so this process should not be killed unless 
# it runs for more then about 6 hours.
#
# The tar.gz files this script creates have 644 permissions set on them. We want this so the
# files are not messed with by people unintentionally. They need to be "nightly" in order to
# harm the files and that requires explicit action on the part of the user.
#
# This test will bail out if /u1/nightly-builds is not available for any reason.
##

#################################################################################
# If u1/nightly-builds is not available, exit immediately
##
cd /u1/nightly-builds
if [ $? = 1 ]; then
  echo "/u1/nightly-builds is not available. This is trouble and I want no part of it. Bailing out.";
  exit 1;
fi

#################################################################################
# This function block checks the local drive and finds any tests older than 3 days then moves
# those off to nbu1.
##

for oldtest in `find . -maxdepth 1 -mtime +2 |grep -v "lost+found" |grep -v "lastrun" |grep -vx "."`; do
  declare oldtestdir=${oldtest#./};
  mv "${oldtest#./}" /permabit/not-backed-up/nightly_runs;
  echo "Directory $oldtestdir has been moved off to nbu1/nightly_runs. Check there for log results." > $oldtestdir.log;
done;

#################################################################################
# This function block checks the backup directory on nbu1 and tars/gzips any test runs older
# than 6 days, then moves that tarball off to dogfood for permanent storage. We declare the
# variable backupname to get rid of the annoying leading "./" on all of the find results.
# The path to the location on dogfood on rd is /permabit/dogfood/nra with dogfood the
# mountpoint and nra a symlink to the nightly_runs_archive dir.
##
cd /permabit/not-backed-up/nightly_runs

for oldbackup in `find . -maxdepth 1 -mtime +5 |grep -v "lost+found" |grep -v "lastrun" |grep -vx "."`; do
  declare backupname=${oldbackup#./} ;
  echo "Creating tar for $backupname then moving it to dogfood." > backup-$backupname.log
  tar -czf $backupname.tar.gz $oldbackup;
  { if [ -s $backupname.tar.gz ]; then
      echo "File created, size not zero, proceeding to move." >> backup-$backupname.log;
    else
      echo "Tar creation failed. Check it manually!" >> backup-$backupname.log;
    fi;
  } ;
  mv $backupname.tar.gz /permabit/dogfood/nra;

#################################################################################
# Verify the move of the tarball. If it is not present and gt 0 then we create a warning
# in a logfile.
##
  { if [ -s /permabit/dogfood/nra/$backupname.tar.gz ]; then
      echo "Nightly run dated $backupname tarred and feathered and moved to dogfood - Have a day." >> backup-$backupname.log;
    else 
      echo "Backup failed on copying tar file for $backupname logs. Empty tar maybe?" >> backup-$backupname.log;
    fi;
  } ;

#################################################################################
# Remove the original folder from nbu1. If permissions prevent this, log that information.
##
  { if [ -w $oldbackup ] && [ -s /permabit/dogfood/nra/$backupname.tar.gz ]; then
      rm -r $oldbackup;
   else 
      echo "Removal of old directory not possible. Check ownership permissions and remove by hand. Script out." >> backup-$backupname.log;
    fi;
  }; 
done;
