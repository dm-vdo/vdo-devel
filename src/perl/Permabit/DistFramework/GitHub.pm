##
# Prepare a GitHub distribution.
#
# @synopsis
#
# use Permabit::Dist::GitHub
#
# Permabit::Dist::GitHub->run();
#
# @description
#
# C<Permabit::Dist::GitHub> prepares sources for distribution to GitHub.
#
# $Id$
##
package Permabit::DistFramework::GitHub;

use strict;
use warnings FATAL => qw(all);
use English;

use File::Basename;
use Permabit::Assertions qw(assertNumArgs);
use Permabit::SystemUtils qw(assertSystem);
use Permabit::Utils qw(makeFullPath);
use Permabit::VersionNumber;

use base qw(Permabit::DistFramework);

my $MANIFEST_FILE = 'src/packaging/github/MANIFEST.yaml';

my %ARGUMENTS = (
  branch           => '=s',
  fromRPMs         => '!',
  kernel           => '=s',
  repositoryPrefix => '=s',
  test             => '!',
);

my $GIT_HUB = 'https://github.com';

######################################################################
# @inherit
##
sub run {
  my ($package) = assertNumArgs(1, @_);
  $package->SUPER::run(arguments  => { %ARGUMENTS },
                       config     => { manifest => $MANIFEST_FILE },
                       mainMethod => \&main);
}

######################################################################
# @inherit
##
sub main {
  my ($self) = assertNumArgs(1, @_);
  $self->SUPER::main();
  if ($self->get('test')) {
    return;
  }

  $self->makeGitCommit();
}

######################################################################
# @inherit
##
sub processChangeLog {
  my ($self, $handle) = assertNumArgs(2, @_);
  my $project;
  my $changeLogs = {};
  while (my $line = $handle->getline()) {
    if ($line =~ /^(\S+):$/) {
      $project = $1;
      next;
    }

    $line =~ s/^\* /- /;
    $changeLogs->{$project} .= $line;
  }
  $self->set('changelog', $changeLogs);
}

######################################################################
# @inherit
##
sub prepareSources {
  my ($self) = assertNumArgs(1, @_);
  if (!$self->get('fromRPMs')) {
    return $self->SUPER::prepareSources();
  }

  my $source = $self->get('source');
  if (!-d $source) {
    die("Source $source does not exist");
  }

  my $rpmGlob = makeFullPath($self->get('source'), '*.src.rpm');
  foreach my $rpm (glob($rpmGlob)) {
    $self->info("unpacking $rpm");
    assertSystem("rpm2cpio $rpm | cpio -idmv");
  }

  foreach my $tgz (glob("*.tgz")) {
    $self->info("unpacking $tgz");
    assertSystem("tar xzf $tgz");
    unlink($tgz);
  }

  foreach my $spec (glob("*.spec")) {
    my ($prefix) = split('\.', $spec);
    assertSystem("mv $spec $prefix-*");
    $self->set("${prefix}Dir", makeFullPath(glob("${prefix}-*")));
  }

  return undef;
}

######################################################################
# @inherit
##
sub readSpecFile {
  my ($self, $fileName) = assertNumArgs(2, @_);
  my $spec = $self->SUPER::readSpecFile($fileName);

  # For GitHub, the spec file is always version 1 since the VDO version changes
  # whenever the spec file does.
  $spec =~ s/(\%define spec_release)\s+\d+/$1 1/s;
  return $spec;
}

######################################################################
# @inherit
##
sub updateSpecFileChangeLog {
  my ($self, $specFileName, $spec, $header) = assertNumArgs(4, @_);

  # Always sanitize the change log.
  my $project = basename($specFileName, '.spec');
  my $changelog = $self->get('changelog');
  my $change = (defined($changelog) ? $changelog->{$project} : undef);
  $change //= '- See ' . $self->makeRepoURL($project) . "\n";
  $spec =~ s/(\%changelog).*$/$1\n$header\n$change/s;
  return $spec;
}

######################################################################
# @inherit
##
sub getSpecFileOutputPath {
  my ($self, $specFileSource) = assertNumArgs(2, @_);
  # Write the spec file to the top of the prepared directory to which it
  # applies.
  my $base = basename($specFileSource);
  return makeFullPath(glob(basename($base, '.spec') . "*"), $base);
}

######################################################################
# Commit changes to local GitHub projects.
##
sub makeGitCommit {
  my ($self) = assertNumArgs(1, @_);
  my $changelogs = $self->get('changelog');
  if (!defined($changelogs)) {
    return;
  }

  my ($marketingVersion, $version) = getGitVersions($self);
  foreach my $project (keys(%{$changelogs})) {
    assertSystem("git clone " . $self->makeRepoURL($project));
    $self->cd($project);
    my $branchName = $self->branchProject($marketingVersion, $version);
    $self->cd('..');
    $self->writeFile("commit.$project",
                     "Version $version\n\n$changelogs->{$project}",
                     mode => 0666);
    my $gitFrom = "$project/.git";
    my $dir     = $self->get("${project}Dir");
    my $gitTo   = "$dir/.git";
    if (!rename($gitFrom, $gitTo)) {
      die("rename($gitFrom, $gitTo) failed: $ERRNO");
    }

    assertSystem("rm -rf $project");
    assertSystem("cd $dir && git add -A && git commit -F ../commit.$project");
    $self->info("To complete the upload for $project, run 'git push origin "
                . "$branchName' in $dir");
  }
}

######################################################################
# Get the version and marketing version for creating github branches.
##
sub getGitVersions {
  my ($self) = assertNumArgs(1, @_);
  if ($self->get('fromRPMs')) {
    # Since we don't have the CURRENT_VERSION_FILE in the RPMs we extracted,
    # we need to derive the versions from the name of the source directory.
    my $source = $self->get('source');
    my (@source) = split('/', $source);
    my $version = pop(@source);
    if ($version !~ /(\d+-\d+-\d+-\d+)$/) {
      die("Can't extract version from source: $source");
    }

    $version = Permabit::VersionNumber->new($1, '-');
    return ($version->shorten(2, 3), $version);
  }

  my $manifest    = $self->get('manifest');
  my $versionFile = $self->getSpecFileVersionFile($manifest->{specFiles});
  return ($versionFile->get('MARKETING_VERSION'),
          $versionFile->get('VERSION'));
}

######################################################################
# Construct a GitHub URL
#
# @param project  The name of the project
#
# @return The URL for cloning the project
##
sub makeRepoURL {
  my ($self, $project) = assertNumArgs(2, @_);
  my @components = ($GIT_HUB);
  my $prefix = $self->get('repositoryPrefix');
  if ($prefix) {
    push(@components, $prefix);
  }

  push(@components, "${project}.git");
  return join('/', @components);
}

######################################################################
# Branch the git project if necessary.
#
# @param branch   The name of the branch if the version being pushed precedes
#                 the latest master version
# @param version  The version being pushed
#
# @return The name of the branch to commit to
##
sub branchProject {
  my ($self, $branch, $version) = assertNumArgs(3, @_);
  my $commandLineBranch = $self->get('branch');
  if (defined($commandLineBranch)) {
    assertSystem("git checkout $commandLineBranch");
    return $commandLineBranch;
  }

  my $shouldBranch         = 0;
  my $branchFrom           = [];

  my $result = assertSystem('git ls-remote');
  foreach my $line (split('\n', $result->{stdout})) {
    if ($line !~ m|^(\S+)\s+refs/tags/(\S+)|) {
      next;
    }

    my ($sha, $tag) = ($1, Permabit::VersionNumber->new($2));
    my $tagBranch   = $self->makeMarketingVersion($tag);
    if ($tag >= $version) {
      if ($tagBranch == $branch) {
        die("Version to push, $version, precedes already pushed version $tag");
      }

      # There is a version which is greater than the version being pushed, so
      # the version being pushed must go on a branch.
      $shouldBranch = 1;
      next;
    }

    if ($tag > $branchFrom->[0]) {
      # This is the newest version we've seen which is older than the version
      # being pushed. If there isn't already a branch to which to push, this is
      # the point where the branch should be made if a branch is needed.
      $branchFrom = [$tag, $sha];
    }
  }

  if (!$shouldBranch) {
    return 'master';
  }

  $result = assertSystem('git branch -r');
  foreach my $line (split('\n', $result->{stdout})) {
    if ($line =~ m|^\s*origin/$branch\s*$|) {
      # The branch we want to push to already exists.
      assertSystem("git checkout $branch");
      return $branch;
    }
  }

  # Make a new branch.
  assertSystem("git checkout -b $branch $branchFrom->[1]");
  return $branch;
}

1;
