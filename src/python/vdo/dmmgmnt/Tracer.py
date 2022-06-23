#
# %COPYRIGHT%
#
# %LICENSE%
#

"""

Tracer - class for tracer instances

$Id$

"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

from .DeviceMapper import DeviceMapper
import gettext
import logging
import os
import sys

gettext.install('dmdevice')

class Tracer(DeviceMapper):
  """Class for tracer.

  Class attributes:
    log (logging.Logger) - logger for this class

  Attributes:
  """

  log = logging.getLogger(os.path.basename(sys.argv[0])
                          + ".Service"
                          + ".DeviceMapper"
                          + ".Tracer")

######################################################################
# Public methods
######################################################################

######################################################################
# Overridden methods
######################################################################
  def __init__(self,
               deviceName,
               storageDevicePath  = None,
               traceSectorCount   = None,
               **kwargs):
    super(Tracer, self).__init__(deviceName,
                                 "pbittracer",
                                 storageDevicePath,
                                 **kwargs)
    self.__traceSectorCount = traceSectorCount

  ####################################################################
  def _makeTableString(self, resolvePath = False):
    return " ".join([super(Tracer, self)._makeTableString(resolvePath),
                     str(self._traceSectorCount())])

######################################################################
# Protected methods
######################################################################
  def _traceSectorCount(self):
    if self.__traceSectorCount is None:
      self.__traceSectorCount = int(self.status().split()[-2])
    return self.__traceSectorCount

######################################################################
# Private methods
######################################################################
