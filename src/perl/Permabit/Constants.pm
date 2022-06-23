##
# Release info
# Constants defined for Permabit modules. Re-exports all constants
# from Permabit::MainConstants, overriding values as needed for
# this code line. For convenience, also defines constants common to
# the perl tree on this code line.
#
# @synopsis
#
#     use Permabit::Constants;
#
#     my $size = 13 * $KB;
#
# $Id$
##
package Permabit::Constants;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use Permabit::MainConstants;

use base qw(Exporter);

our $VERSION = "1.1";
our @EXPORT = qw(
  $DEFAULT_ALBIREO_PORT
  $DEFAULT_BLOCK_SIZE
  $DEFAULT_CHUNK_SIZE
  $DEFAULT_DISCARD_RATIO
  $DEFAULT_INDEX_DIR
  $DEFAULT_JIRA_PROJECT_KEY
  $DEFAULT_KVDO_DIR
  $DEFAULT_MAX_REQUESTS_ACTIVE
  $DEFAULT_RAID_INDEX_DIR
  $MACHINE_ALWAYS_DOWN
  $PHYSICAL_BLOCK_SIZE
  $RAID_DIR
  $SECTORS_PER_BLOCK
  $SLAB_BITS_LARGE
  $SLAB_BITS_SMALL
  $SLAB_BITS_TINY
);

# Re-export all constants exported by Permabit::MainConstants
{
  push(@EXPORT, @Permabit::MainConstants::EXPORT);
}

# constants relevant to uds and vdo
our $DEFAULT_CHUNK_SIZE          = 4 * $KB;
our $PHYSICAL_BLOCK_SIZE         = $DEFAULT_CHUNK_SIZE;
our $DEFAULT_BLOCK_SIZE          = $PHYSICAL_BLOCK_SIZE;
our $DEFAULT_ALBIREO_PORT        = 8000;
our $DEFAULT_MAX_REQUESTS_ACTIVE = 2048;
our $DEFAULT_DISCARD_RATIO       = 0.75;

# VDO slabBits settings.  SLAB_BITS_LARGE is used for performance testing.
# SLAB_BITS_SMALL is the smallest size that will always work for any host
# reserved using RSVP by a test.  SLAB_BITS_TINY is even smaller, and can be
# used by tests that explicitly use a logical device with an lvmSize setting,
# or a VDO device with a physicalSize setting.
our $SLAB_BITS_LARGE = 23;
our $SLAB_BITS_SMALL = 17;
our $SLAB_BITS_TINY  = 15;

our $SECTORS_PER_BLOCK = $PHYSICAL_BLOCK_SIZE / $SECTOR_SIZE;

our $RAID_DIR               = '/mnt/raid0';
our $DEFAULT_INDEX_DIR      = '/u1/albireo';
our $DEFAULT_KVDO_DIR       = '/mnt/VDOdir';
our $DEFAULT_RAID_INDEX_DIR = "$RAID_DIR/albireo";

# Add any project- or branch-specific constants below, listing them in @EXPORT
# above. Values from MainConstants can also be overridden, but be aware that
# the value defined here won't affect any derived values in MainConstants.

# Release info
# These must be listed in order of newest release to oldest.
our @RELEASE_MAPPINGS = (
  # Albireo probably doesn't belong here because it's not an OS,
  # it's a configuration.
  q(albireo   x   lenny,precise,rhel6,sles11sp2,sles11sp3,squeeze,wheezy39,wheezy310   albireo),
);
our @CODENAMES = map { (split(/\s+/, $_))[0] } @RELEASE_MAPPINGS;
our $CODENAME = $CODENAMES[0];

our $DEFAULT_JIRA_PROJECT_KEY = 'ALB';

# This machine doesn't exist, but there is a DNS entry for it (for testing)
our $MACHINE_ALWAYS_DOWN = "always-down.permabit.com";

1;
