##
# Mix-in for ScriptFramework which provides methods for managing version
# numbers and CURRENT_VERSION files.
#
# @synopsis
#
# Permabit::ScriptFramework->run(features => { Versioned => 1 }, ...);
#
# sub main {
#   $framework->makeMarketingVersion('PROJECT_VERSION');
# }
##
package Permabit::ScriptFramework::Versioned;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Storable qw(dclone);

use Permabit::Assertions qw(
  assertMinMaxArgs
  assertNumArgs
);
use Permabit::CurrentVersionFile;
use Permabit::SystemUtils qw(assertSystem);
use Permabit::VersionNumber;

use base qw(Exporter);

our @EXPORT = qw(
  areSameVersionFiles
  copyVersionFile
  getVersionFileValue
  readVersionFile
  makeMarketingVersion
  setMarketingVersion
  setVersionFileValue
  writeVersionFile
);

######################################################################
# Check whether two version files contain the same key-value pairs for the
# current project.
#
# @param framework  The script framework
# @param a          The name of the property containing the first version file
# @param b          The name of the property containing the second version file
#
# @return true if each of the two specified version files contain the same
#              key-value pairs for the current project
##
sub areSameVersionFiles {
  my ($framework, $a, $b) = assertNumArgs(3, @_);
  return ($framework->get($a) == $framework->get($b));
}

######################################################################
# Deep copy a version file object from one property to another property.
#
# @param framework  The script framework
# @param source     The name of the property containing the source version file
# @param dest       The name of the property to hold the destination version
#                   file
#
# @return The copied version file object
##
sub copyVersionFile {
  my ($framework, $source, $dest) = assertNumArgs(3, @_);
  return $framework->set($dest, dclone($framework->get($source)));
}

######################################################################
# Read a version file and store its contents in a property.
#
# @param  framework  The script framework
# @param  path       The path to the file to read
# @param  name       The name of the property in which to store the file
# @oparam project    The name of the current project, only necessary if the
#                    version file contains multiple projects
#
# @return The version file object
##
sub readVersionFile {
  my ($framework, $path, $name, $project) = assertMinMaxArgs(3, 4, @_);
  my $handle = $framework->openHandle($path);
  return $framework->set($name, Permabit::CurrentVersionFile->read($handle,
                                                                   $project));
}

######################################################################
# Write a version file stored in a property.
#
# @param framework  The script framework
# @param path       The path of the file to write
# @param name       The name of the property of the version file to write
##
sub writeVersionFile {
  my ($framework, $path, $name) = assertNumArgs(3, @_);
  $framework->get($name)->write($framework->openHandle(">$path"));
}

######################################################################
# Get the marketing version for a version.
#
# @param framework  The script framework
# @param version    The version
#
# @return The marketing version of the specified version
##
sub makeMarketingVersion {
  my ($framework, $version) = assertNumArgs(2, @_);
  return $version->shorten(2, 3);
}

######################################################################
# Set the marketing version of a version file object from the version
# in that object.
#
# @param framework    The script framework
# @param versionFile  The property containing the version file object
#
# @return The new marketing version which was set
##
sub setMarketingVersion {
  my ($framework, $versionFile) = assertNumArgs(2, @_);
  $versionFile = $framework->get($versionFile);
  my $version = $versionFile->get('VERSION');
  return $versionFile->set('MARKETING_VERSION',
                           $framework->makeMarketingVersion($version));
}

######################################################################
# Get a value from a version file object stored in a property.
#
# @param framework  The script framework
# @param name       The property containing the version file object
# @param key        The key to fetch from the version file object
#
# @return The value for the specified key in the named version file object
##
sub getVersionFileValue {
  my ($framework, $name, $key) = assertNumArgs(3, @_);
  return $framework->get($name)->get($key);
}

######################################################################
# Set a value for a key in a version file object stored in a property.
#
# @param framework  The script framework
# @param name       The property containing the version file object
# @param key        The key to set in the version file object
# @param value      The value to set
#
# @return The value which was set
##
sub setVersionFileValue {
  my ($framework, $name, $key, $value) = assertNumArgs(4, @_);
  return $framework->get($name)->set($key, $value);
}

1;
