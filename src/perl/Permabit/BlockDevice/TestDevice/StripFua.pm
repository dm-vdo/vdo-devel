##
# Perl object that represents a block device that removes the FUA and
# FLUSH bits from any write requests
#
# $Id$
##
package Permabit::BlockDevice::TestDevice::StripFua;

use strict;
use warnings FATAL => qw(all);
use English qw(-no_match_vars);

use base qw(Permabit::BlockDevice::TestDevice::Fua);

# The stripfua device is just fua with a different frequency setting.

########################################################################
# @paramList{new}
our %BLOCKDEVICE_PROPERTIES
  = (
     # @ple fua frequency (0 implies stripping all flushes and fuas)
     fuaFrequency => 0,
    );
##

1;
