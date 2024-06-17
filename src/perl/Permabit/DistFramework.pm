##
# Framework for distribution scripts.
#
# @synopsis
#
# use Permabit::DistFramework
#
# sub main {
# }
#
# @description
#
# C<Permabit::DistFramework> provides a framework for building scripts which
# prepare sources for distribution based on a manifest file. The format of
# the manifest file is documented at the end of this module.
#
# All scripts should take one positional argument, which is the directory
# containing the sources to be prepared.
#
# $Id$
##
package Permabit::DistFramework;

use strict;
use warnings FATAL => qw(all);
use English;

use Carp qw(confess);
use Cwd qw(cwd);
use File::Basename;
use File::Spec;
use POSIX qw(strftime);

use Pdoc::Generator qw(pdoc2usage);
use Permabit::Assertions qw(
  assertMinArgs
  assertMinMaxArgs
  assertNumArgs
);
use Permabit::SystemUtils qw(assertSystem);
use Permabit::Utils qw(
  getYamlHash
  makeFullPath
);
use Permabit::VersionNumber;

use base qw(Permabit::ScriptFramework);

my %ARGUMENTS = (
  author    => '=s',
  changelog => '=s',
  debug     => '!',
  dest      => '=s',
  manifest  => '=s',
);

my %DEFAULTS = (
  author => 'Red Hat VDO Team <dm-devel@lists.linux.dev>',
  dest => '.'
);


######################################################################
# @inherit
##
sub run {
  my ($package, %parameters) = assertMinArgs(3, @_);
  my $arguments = delete($parameters{arguments}) // {};
  if (defined($arguments)) {
    foreach my $argument (keys(%ARGUMENTS)) {
      if (!defined($arguments->{$argument})) {
        $arguments->{$argument} = $ARGUMENTS{$argument};
      }
    }
  }

  $parameters{commandLineArguments}
    = [map { $_ . $arguments->{$_} } keys(%{$arguments})];
  $parameters{argParser}                //= \&parseARGV;
  $parameters{debugEnvironmentVariable} //= 'DEBUG_DIST_SCRIPT';
  $parameters{features}->{Versioned}      = 1;
  $parameters{mainMethod}               //= \&main;

  foreach my $default (keys(%DEFAULTS)) {
    $parameters{config}->{$default} //= $DEFAULTS{$default};
  }

  $package->SUPER::run(%parameters);
}

######################################################################
# Parse remaining command line after the named arguments have been parsed.
##
sub parseARGV {
  my ($self) = assertNumArgs(1, @_);
  if (scalar(@ARGV) != 1) {
    pdoc2usage();
  }

  $self->set('source', $ARGV[0]);
}

######################################################################
# The main body.
##
sub main {
  my ($self) = assertNumArgs(1, @_);
  umask(0);
  $self->readChangeLogs();
  $self->set('relativeTo', cwd());
  $self->goToDestination();

  my $manifest = $self->prepareSources();
  if (!$manifest) {
    return;
  }

  $self->readLicense($manifest);
  $self->set('copyright', $manifest->{copyright});

  my $tarballs = $manifest->{tarballs};
  foreach my $tarball (keys(%{$tarballs})) {
    my $tarballManifest = $tarballs->{$tarball};
    $self->getVersions($tarballManifest);
    $self->makeTarballDir($tarball, $tarballManifest);
    $self->populate($tarball, $tarballManifest->{sources});
    $self->makeTarball($tarball);
  }

  $self->makeSpecfiles($manifest);
}

######################################################################
# Read the file of change log entries if there is one.
##
sub readChangeLogs {
  my ($self) = assertNumArgs(1, @_);
  my $changeLogFile = $self->get('changelog');
  if (!defined($changeLogFile)) {
    return;
  }

  my $handle = $self->openHandle($changeLogFile);
  $self->processChangeLog($handle);
  $handle->close();
}

######################################################################
# Process a change log file
#
# @param handle  The handle from which to read the file
#
# @return The processed file contents
##
sub processChangeLog {
  my ($self, $handle) = assertNumArgs(2, @_);
  die(ref($self) . " does not implement processChangeLog()");
}

######################################################################
# Go to the destination directory, making it if necessary.
##
sub goToDestination {
  my ($self) = assertNumArgs(1, @_);
  my $destination = $self->getPath('dest');
  if (! -d $destination) {
    $self->makeDirectory($destination,
                         description => 'dest dir',
                         cleanup     => { onError  => 1,
                                          notDebug => 1,
                                        },
                        );
  }

  $self->cd($destination);
}

######################################################################
# Get a path.
#
# @param  name       The name of the property containing the path
# @oparam additional Optional additional path components to be added to
#                    the specified property
#
# @return The path, made absolute
##
sub getPath {
  my ($self, $name, @additional) = assertMinArgs(2, @_);
  my $path = $self->get($name);
  if (!defined($path)) {
    die("No property with name: $name");
  }

  if (!File::Spec->file_name_is_absolute($path)) {
    $path = $self->set($name, makeFullPath($self->get('relativeTo'), $path));
  }

  return makeFullPath($path, @additional);
}

######################################################################
# Prepare source trees by untarring if necessary, loading the manifest,
# and locating any additional trees.
#
# @return The manifest
##
sub prepareSources {
  my ($self)       = assertNumArgs(1, @_);
  my $source       = getPath($self, 'source');
  my $tree         = $self->untarSourceTree($source, 'source');
  my $manifestFile = makeFullPath($tree, $self->get('manifest'));
  $self->set('postProcessorDir', dirname($manifestFile));

  my $manifest = $self->readManifest($manifestFile);
  $self->set($manifest->{treeName}, $tree);

  my $otherTrees = $manifest->{otherTrees};
  if ($otherTrees) {
    foreach my $treeName (keys(%{$otherTrees})) {
      my $otherManifest = $otherTrees->{$treeName};
      my $sourcePath
        = $self->readFile(makeFullPath($tree,
                                       $otherManifest->{sourcePathFrom}));
      chomp($sourcePath);
      my $sourceFile = makeFullPath($sourcePath,
                                    $otherManifest->{sourceFile});
      $self->set($treeName, $self->untarSourceTree($sourceFile, $treeName));
    }
  }

  return $self->set('manifest', $manifest);
}

######################################################################
# Untar a source tree if necessary.
#
# @param source     The tree
# @param name       The name of the directory for the untarred source
#
# @return The location of the untarred tree
##
sub untarSourceTree {
  my ($self, $source, $name) = assertNumArgs(3, @_);
  if (-d $source) {
    return $source;
  }

 if (!( -e $source) && (-d dirname($source))) {
    return dirname($source);
  }

  if ($source !~ /\.tgz/) {
    die("Unknown source archive format for $name: $source\n");
  }

  $self->info("Getting source for $name from $source\n");
  my $tree = $self->getPath('dest', $name);
  $self->makeDirectory($tree,
                       description => "$name source dir",
                       cleanup     => { notDebug => 1, },
                      );
  assertSystem("cd $tree; tar xzf $source");
  return $tree;
}

######################################################################
# Load the manifest.
#
# @param manifestFile  The file from which to read the manifest
#
# @return The manifest
##
sub readManifest {
  my ($self, $manifestFile) = assertNumArgs(2, @_);
  my $manifest = getYamlHash($manifestFile);
  my $include  = delete $manifest->{manifest};
  if (!defined($include)) {
    return $manifest;
  }

  if (!File::Spec->file_name_is_absolute($include)) {
    $include = makeFullPath(dirname($manifestFile), $include);
  }

  $self->info("Including manifest $include");
  return modifyHash($self->readManifest($include), $manifest);
}

######################################################################
# Modify a hash based on another hash. Examines each key of the modifier
# hash.  If that key starts with a '+' the key and its value will be added
# to the original hash overwriting the original hash's key value if it exists.
# If that key starts with a '-', the values under that key will be removed
# from the original hash.
#
# @param hash      The hash to be modified
# @param modifier  The hash of modifications
#
# @return The modified hash
##
sub modifyHash {
  my ($hash, $modifier) = assertNumArgs(2, @_);
  foreach my $key (keys(%{$modifier})) {
    # Require the key name to abut the any prefix character and not be empty.

    # Replace value?
    if ($key =~ /^\+(\S.*)$/) {
      $hash->{$1} = $modifier->{$key};
      next;
    }

    # Delete value:
    if ($key =~ /^\-(\S.*)/) {
      delete $hash->{$1};
      next;
    }

    # The modifier value must be a hash in order to recurse (recursing on
    # arrays is not supported).  Additionally the target hash must not have a
    # matching key or if it does the value must also be a hash.
    if (ref($modifier->{$key}) ne "HASH") {
      confess("can't recurse; modifier '$key' value not a hash: "
              . ref($modifier->{$key}));
    }
    if (defined($hash->{$key}) && (ref($hash->{$key}) ne "HASH")) {
      confess("can't recurse; target '$key' value not a hash: "
              . ref($hash->{$key}));
    }

    $hash->{$key} = modifyHash($hash->{$key} , $modifier->{$key});
  }

  return $hash;
}

######################################################################
# Read the license file.
#
# @param manifest   The manifest
##
sub readLicense {
  my ($self, $manifest) = assertNumArgs(2, @_);
  my $licenseFile = $manifest->{licenseFile};
  if (!$licenseFile) {
    die("No license file specified.");
  }

  my $licensePath = $self->getPath($licenseFile->{tree}, $licenseFile->{file});
  $self->set('license', [$self->readFile($licensePath)]);
}

######################################################################
# Read all version files specified for a given tarball.
#
# @param manifest   The tarball manifest
##
sub getVersions {
  my ($self, $manifest) = assertNumArgs(2, @_);
  my $versionFiles = $manifest->{versionFiles};
  if (!defined($versionFiles)) {
    return;
  }

  foreach my $tree (keys(%{$versionFiles})) {
    my $versionFile
      = $self->readVersionFile($self->getPath($tree, $versionFiles->{$tree}),
                               "${tree}VersionFile");
    $self->set(uc($tree) . 'Version', $versionFile->get('VERSION'));
  }
}

######################################################################
# Make the top-level directory for a tarball.
#
# @param  tarball    The name of the tarball
# @param  manifest   The manifest for the tarabll
# @oparam cleanup    The cleanup setting for the tarball directory
##
sub makeTarballDir {
  my ($self, $tarball, $manifest, $cleanup) = assertMinMaxArgs([], 3, 4, @_);
  my $version
    = $self->getVersionFileValue($manifest->{versionFrom} . 'VersionFile',
                                 'VERSION');
  my $dir     = $self->set("${tarball}Dir",
                           $self->getPath('dest', "${tarball}-$version"));
  my @args = (description => "$tarball tarball directory",
              preDelete   => 1);
  if (defined($cleanup)) {
    push(@args, cleanup => { notDebug  => 1, });
  }
  $self->makeDirectory($dir, @args);
}

######################################################################
# Populate a tarball directory
#
# @param tarballType  The tarball type (user or kernel)
# @param manifest     The manifest for the given tarball type
##
sub populate {
  my ($self, $tarballType, $manifest) = assertNumArgs(3, @_);
  # Regular expression to match 'KERNEL_VERSION(N, N, N)'
  my $re = 'KERNEL_VERSION\((\d+),\s*(\d+)(?:,\s*(\d+))\)';
  my $kernel = $self->get('kernel');
  if ($kernel) {
    # Convert version N.N.N to a single integer value
    $kernel =~ s/(\d+)\.(\d+)(?:\.(\d+))?/($1<<16)+($2<<8)+$3/ge;
  }
  my $tarballDir = $self->getPath("${tarballType}Dir");
  my $postProcessorDir = $self->get('postProcessorDir');

  foreach my $sourceTree (keys(%{$manifest})) {
    my $sourceDir      = $self->get($sourceTree);
    my $sourceManifest = $manifest->{$sourceTree};
    foreach my $sourcePath (keys(%{$sourceManifest})) {
      my $item    = $sourceManifest->{$sourcePath};
      my $destDir = makeFullPath($tarballDir, $item->{dest});
      my $opener = '';
      my $suffix = '';
      if ($item->{undefines} || $item->{defines}) {
        my @undefines = map({ "-U$_" } (defined($item->{undefines})
                                        ? @{$item->{undefines}}
                                        : ()));
        my @defines = map({ "-D$_" } (defined($item->{defines})
                                        ? @{$item->{defines}}
                                        : ()));
        if ($kernel and grep { $_ eq "-D__KERNEL__" } @defines) {
          # Replace KERNEL_VERSION(N, N, N) with its integer
          # equivalent and define LINUX_VERSION_CODE to generate
          # source for a specific kernel version
          $opener = "perl -pe 's/$re/(\$1<<16)+(\$2<<8)+\$3/ge' ";
          push(@defines, "-DLINUX_VERSION_CODE=${kernel}");
          $suffix = join(' ', " |", 'unifdef', @defines, @undefines, '|');
        } else {
          $opener = join(' ', 'unifdef', @defines, @undefines, '');
          $suffix   = ' |';
        }
      }
      my @excludes = (defined($item->{excludes})
                      ? @{$item->{excludes}}
                      : ());
      my $postProcessor = (defined($item->{postProcessor})
                           ? makeFullPath($postProcessorDir,
                                          $item->{postProcessor})
                           : undef);
      foreach my $source (@{$item->{sources}}) {
        $self->copyFiles(makeFullPath($sourceDir, $sourcePath),
                         $source,
                         makeFullPath($destDir, dirname($source)),
                         $tarballType,
                         $opener,
                         $suffix,
                         \@excludes,
                         $postProcessor);
      }
    }
  }
}

######################################################################
# Copy a set of files into the tarball directory.
#
# @param sourceDir      The source directory to copy from
# @param files          The set of files to copy, either as a glob or a mapping
#                       as described in the manifest YAML file. The sourceDir
#                       will be searched recursively if it contains
#                       sub-directories
# @param destDir        The destination directory to copy to
# @param tarballType    The tarball type (user or kernel)
# @param opener         A pre-filter when reading the file to be copied, or
#                       an empty string to indicate no filter
# @param suffix         A suffix ending with a pipe character which is required
#                       if an opener is specified, and which can specify
#                       additional processing after the opener and before the
#                       pipe character; or an empty string if no opener is
#                       specified.
# @param excludes       Files not to copy
# @param postProcessor  If defined, a script to run on each file after it is
#                       copied. The script will be passed the path of the
#                       destination file
##
sub copyFiles {
  my ($self, $sourceDir, $files, $destDir, $tarballType, $opener, $suffix,
      $excludes, $postProcessor) = assertNumArgs(9, @_);
  my $makeDir = 1;
  my $rename  = 0;
  if ($files =~ /^(.*)->(.*)$/) {
    $files  = $1;
    $rename = $2;
  }

  my @files    = glob(makeFullPath($sourceDir, $files));
  foreach my $exclude (@{$excludes}) {
    @files = grep { basename($_) ne $exclude } @files;
  }

  my $copyright = $self->get('copyright');
  foreach my $file (@files) {
    if (! -e $file) {
      die("Source $file doesn't exist.");
    }

    if (-d $file) {
      $self->copyFiles($file, $files,
                       makeFullPath($destDir, basename($file)), $tarballType,
                       $opener, $suffix, $excludes, $postProcessor);
      next;
    }

    if ($makeDir) {
      $self->makeDirectory($destDir);
      $makeDir = 0;
    }

    my $content = $self->readFile("${opener}${file}${suffix}");
    $content =~ s/\n[^\n]*\n[^\n]*ATTENTION!!!.*BY HAND//gs;
    $content =~ s/\%COPYRIGHT\%/$copyright/gs;

    if ($content =~ /\n([^\n]*)\%LICENSE\%/s) {
      my $license = join($1, @{$self->get('license')});
      $license =~ s/\s+\n/\n/g;
      $license =~ s/\n$//;
      $content =~ s/\%LICENSE\%/$license/gs;
    }

    if ($tarballType eq "kvdo") {
      $content =~ s/\" PRIu64 \"/llu/g;
      $content =~ s/\" PRIu64,/llu\",/g;
    }

    $content =~ s/\%%(\w+)\%%/$self->get($1)/gse;

    my $destFile = ($rename ? $rename : basename($file));
    my $destPath = makeFullPath($destDir, $destFile);
    my $output   = $self->createFile($destPath,
                                     mode => ((-x $file) ? 0777 : 0666),
                                    );
    $output->print($content);
    $output->close();
    if (defined($postProcessor)) {
      assertSystem("$postProcessor $destPath");
    }
  }
}

######################################################################
# Make a tarball
#
# @param tarballType  The tarball type (user or kernel)
##
sub makeTarball {
  my ($self, $tarballType) = assertNumArgs(2, @_);
  # By default, there's nothing to do here
}

######################################################################
# Extract spec files.
#
# @param manifest   The manifest
##
sub makeSpecfiles {
  my ($self, $manifest) = assertNumArgs(2, @_);
  my $specFiles = $manifest->{specFiles};
  if (!defined($specFiles)) {
    return;
  }

  my $tree        = $self->get($specFiles->{tree});
  my $versionFile = $self->getSpecFileVersionFile($specFiles);
  my $version     = $versionFile->get('VERSION');
  foreach my $file (@{$specFiles->{files}}) {
    my $spec = $self->readSpecFile(makeFullPath($tree, $file));
    if ($spec !~ /\%define spec_release\s+(\d+)/) {
      die("spec file $file does not define spec_release\n");
    }
    my $release = $1;
    $spec =~ s/\@VERSION\@/$version/sg;
    my $date = $ENV{SPEC_FILE_DATE} // strftime('* %a %b %d %Y', localtime());
    my $header = join(' - ',
                      $date,
                      $self->get('author'),
                      "${version}-$release");
    $spec = $self->updateSpecFileChangeLog($file, $spec, $header);
    $self->writeFile($self->getSpecFileOutputPath($file), $spec, mode => 0666);
  }
}

######################################################################
# Get the version file to use for versioning spec files.
#
# @param specFiles  The spec file manifest
#
# @return The version file object to use for versioning spec files
##
sub getSpecFileVersionFile {
  my ($self, $specFiles) = assertNumArgs(2, @_);
  return $self->get("$specFiles->{tree}VersionFile");
}

######################################################################
# Read a spec file.
#
# @param fileName  The name of the file to read
#
# @return The contents of the spec file
##
sub readSpecFile {
  my ($self, $fileName) = assertNumArgs(2, @_);
  return $self->readFile($fileName);
}

######################################################################
# Update the change log in a spec file.
#
# @param specFileName  The name of the spec file
# @param spec   The contents of the spec file
# @param header The header for the new change log entry
#
# @return The updated spec file contents
##
sub updateSpecFileChangeLog {
  my ($self, $specFileName, $spec, $header) = assertNumArgs(4, @_);
  die(ref($self) . " does not implement updateSpecFileChangeLog()");
}

######################################################################
# Get the output path for a spec file.
#
# @param specFileSource  The source spec file
#
# @return The path for writing prepared the spec file
##
sub getSpecFileOutputPath {
  my ($self, $specFileSource) = assertNumArgs(2, @_);
  # Write the spec file to the current destination directory.
  return basename($specFileSource);
}

1;

#
# This section docments the manifest file syntax. Manifest files are YAML.
#
# There are six top-level keys:
#
# copyright: The copyright string to use
#
# licenseFile: The license file, specified via two sub-keys:
#
#                tree: The source tree which contains the license file.
#                file: The path to the license file relative to the specified
#                      source tree.
#
# manifest: Takes the path to another manifest file which should be included
#           in this one. If specified, the rest of this manifest will be
#           interpreted as changes to the included manifest. Changes work as
#           follows:
#             A key may be prefixed with a '+' or '-' without intervening
#             whitespace.  The lack of whitespace provides consistency of
#             specification as well as YAML format requirements; a '-' with
#             whitespace after is interpreted as a list entry and the '-' is
#             not preserved.  Also such a use of '-' is a YAML format error if
#             at the same level as other keys.
#
#             Values under a key starting with a '+' will replace those (or be
#             added if non-existent) in the included manifest.
#
#             A key starting with a '-' and everything under it will be deleted
#             from the included manifest (sadly, to be valid YAML, the '-' key
#             must have a value, but that value is ignored).
#
#             Any other key is used to cause the modifier to recurse into the
#             values for that key if the key references a hash.  The target
#             being modified must not have a matching key or if it does the
#             value must also be a hash.
#
#             Recursion into arrays is not supported.
#
#             Given a manifest m1.yaml containing:
#
#               foo:
#                 bar: 1
#                 baz: 2
#                 bletch:
#                   salt: pepper
#                   waldo: quux
#                 kindling:
#                   - shadrach
#                   - meshach
#
#              and another manifest containing:
#
#                manifest: m1.yaml
#                foo:
#                  +boozle:
#                     a: 1
#                     b: 2
#                  bletch:
#                    -salt: .
#                    +foobie: 7
#
#              loading this second manifest would result in the following hash:
#
#              {
#                foo => {
#                         bar    => 1,
#                         baz    => 2,
#                         bletch => {
#                                     waldo  => quux,
#                                     foobie => 7,
#                                   },
#                         boozle => {
#                                     a => 1,
#                                     b => 2,
#                                   },
#                         kindling => [ shadrach, meshach ],
#                       },
#              }
#
#              Note that a side-effect of including a manifest is that the
#              top-level manifest key is not present in the resulting hash.
#
# otherTrees: If present, this key indicates that other source trees are to be
#             used in addition to the one named on the command line. Each
#             sub-key is the name by which the manifest will refer to the other
#             tree. Under each tree name, there should be two keys:
#
#               sourcePathFrom: Defines a file in the original source tree
#                               which has the name of the directory containing
#                               the other tree or tarball
#               sourceFile: The name of the file or directory of the actual
#                               source tree within the directory specified in
#                               sourcePathFrom.
#
# specFiles: If present, indicates that spec files should be extracted from
#            the source trees. They will be updated with the current version
#            and placed in the same directory as the source tarballs. Under
#            this key should be two keys:
#              tree:  The source tree which holds the spec files
#              files: The list of spec files relative to that tree
#
# tarballs: This key defines the set of tarballs to make, each key under it
#           is the basename of the tarball (i.e. a key of "foo" will result
#           in a tarball named "foo-<version>.tgz"). Under each tarball key
#           are three supported keys:
#
#           versionFiles: This key defines the files from which versions are
#                         obtained. It contains sub-keys, one for each source
#                         tree, each with a value naming the version file for
#                         that source tree (defined relative to the top of the
#                         tree).
#           versionFrom:  This key defines which source tree's version should
#                         be used as the version of the tarball.
#           sources:      This key is used to define the set of files which go
#                         into the tarball. It contains keys for each source
#                         tree.
#                         Under each source tree key are keys which
#                         name a directory relative to the top of that source
#                         tree.
#                         Under each directory tree are keys which what to copy
#                         from that source dirctory:
#
#                         dest:     The value of this key is the directory,
#                                   relative to the top of the tarball to copy
#                                   to.
#                         sources:  This key contains a list of paths or
#                                   renamings. A path can be a file, glob, or
#                                   directory. If a glob, all matching files
#                                   will be copied. If a directory, the entire
#                                   directory tree under that directory will
#                                   also be copied. A renaming is specified
#                                   as <source-name>-><dest-name>. It can only
#                                   be a single file, and the file will be
#                                   renamed when it is copied.
#
#                         excludes: A list of files or directories to be
#                                   excluded from those in sources.
#
#                         undefines: A list of symbols to be stripped by
#                                    passing copied files through unifdef.
#
#                         defines:   A list of symbols to be defined by
#                                    passing copied files through unifdef.
#
#                         postProcessor: A script to run on each file right
#                                        after it has been copied. If not an
#                                        absolute path it is interpreted as a
#                                        path relative to the directory of the
#                                        top level manifest.
#
# treeName: The value of this key is the name by which the rest of the manifest
#           file will refer to the source tree which contains the manifest
#           (i.e. this name will applied to the tree which is specified on the
#           command line).
#
