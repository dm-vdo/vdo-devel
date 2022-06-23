#
# %COPYRIGHT%
#
# %LICENSE%
#

"""

Corruptor - class for corruptor instances

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
import re
import sys

gettext.install('dmdevice')

class Corruptor(DeviceMapper):
  """Class for corruptor.

  Class attributes:
    log (logging.Logger) - logger for this class

  Attributes:
  """

  log = logging.getLogger(os.path.basename(sys.argv[0])
                          + ".Service"
                          + ".DeviceMapper"
                          + ".Corruptor")

######################################################################
# Public methods
######################################################################

######################################################################
# Overridden methods
######################################################################
  def __init__(self,
               deviceName,
               storageDevicePath = None,
               **kwargs):
    super(Corruptor, self).__init__(deviceName,
                                    "pbitcorruptor",
                                    storageDevicePath,
                                    **kwargs)

  ####################################################################
  def _configureMessages(self, **kwargs):
    if (kwargs["readFrequency"] is None) or (kwargs["writeFrequency"] is None):
      (readFrequency, writeFrequency) = self._currentReadWriteFrequencies()
      if kwargs["readFrequency"] is None:
        kwargs["readFrequency"] = readFrequency
      if kwargs["writeFrequency"] is None:
        kwargs["writeFrequency"] = writeFrequency

    messages = []
    for ioType in ["read", "write"]:
      option = kwargs.get(ioType)
      if option is None:
        continue
      if option in ["disable", "enable"]:
        messages.append("{option} {ioType}".format(option = option,
                                                   ioType = ioType))
      else:
        messages.append("parameters {ioType} {option} {freq}".format(
                          ioType = ioType, option = option,
                          freq = str(kwargs[ioType + "Frequency"])))

    superMessages = super(Corruptor, self)._configureMessages(**kwargs)
    superMessages.extend(messages)
    return superMessages

  ####################################################################
  def _mapStatusToControlMessage(self):
    status = self._serviceStatus()

    messages = []

    # Right split on the table with the path resolved.
    (before, sep, remaining) = status.rpartition(self._makeTableString(True))
    remaining = remaining.strip()

    for ioType in ["read", "write"]:
      state = None
      corruptType = None
      frequency = None

      # Get the state.
      match = re.match(r'{ioType}\s+(on|off)\s+'.format(ioType = ioType),
                       remaining)
      if match is None:
        raise RuntimeError(_("could not find state of {service}").format(
                            service = self._serviceName()))
      state = "enable" if match.group(1) == "on" else "disable"
      remaining = re.split(r'{ioType}\s+(on|off)\s+'.format(ioType = ioType),
                           remaining,
                           maxsplit = 1)[-1]

      # Get the type.
      match = re.match(r'([A-Za-z]+)\s+', remaining)
      if match is None:
        raise RuntimeError(_("could not find type of {service}").format(
                            service = self._serviceName()))
      corruptType = match.group(1)
      remaining = re.split(r'([A-Za-z]+)\s+', remaining, maxsplit = 1)[-1]

      # Get the frequency.
      match = re.match(r'(\d+)\s*', remaining)
      if match is None:
        raise RuntimeError(_("could not find frequency of {service}").format(
                            service = self._serviceName()))
      frequency = match.group(1)
      remaining = re.split(r'(\d+)\s*', remaining, maxsplit = 1)[-1]

      # Add the config messages.
      if state == "enable":
        messages.append(" ".join([state, ioType, corruptType, frequency]))
      else:
        messages.append(" ".join([state, ioType]))
        messages.append(" ".join(["parameters", ioType, corruptType,
                                  frequency]))

    # Return the message string.
    device = self._deviceName()
    return ";".join([" ".join(["dmsetup", "message", device, "0", message])
                      for message in messages])

######################################################################
# Protected methods
######################################################################
  def _currentReadWriteFrequencies(self):
    """Returns a tuple of the current values (as integers) for the device's
       read and write frequencies.

       Returns:
        (readFrequncy, writeFrequency)
    """
    status = self._serviceStatus()

    # Right split on the table with the path resolved.
    (before, sep, remaining) = status.rpartition(self._makeTableString(True))
    regex = re.compile(r'read\s+(on|off)\s+[A-Za-z]+\s+(\d+)\s+'
                       + r'write\s+(on|off)\s+[A-Za-z]+\s+(\d+)\s*')
    match = re.match(regex, remaining.strip())
    if match is None:
      raise RuntimeError(
        _("could not find read and/or write frequency of {service}").format(
          service = self._serviceName()))
    return (int(match.group(2)), int(match.group(4)))

######################################################################
# Private methods
######################################################################
