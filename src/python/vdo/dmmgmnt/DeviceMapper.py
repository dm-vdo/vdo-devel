#
# %COPYRIGHT%
#
# %LICENSE%
#

"""

DeviceMapper - base class for device mapper instances

$Id$

"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

from vdo.utils import Command, CommandError, Service
import gettext
import logging
import os
import re
import stat
import sys

gettext.install('dmdevice')

########################################################################
class DeviceMapperInvalidParameter(Exception):
  """Exception raised when given an invalid parameter.
  """
  def __init__(self, msg = None, *args, **kwargs):
    super(DeviceMapperInvalidParameter, self).__init__(*args, **kwargs)
    if msg is None:
      msg = _("invalid parameter")
    self._msg = msg

  ######################################################################
  def __str__(self):
    return self._msg

########################################################################
class DeviceMapperMissingParameter(Exception):
  """Exception raised when a required parameter is missing.
  """
  def __init__(self, msg = None, *args, **kwargs):
    super(DeviceMapperMissingParameter, self).__init__(*args, **kwargs)
    if msg is None:
      msg = _("missing parameter")
    self._msg = msg

  ######################################################################
  def __str__(self):
    return self._msg

########################################################################
class DeviceMapper(Service):
  """Base class for device mapper init.d script maintenance.

  Class attributes:
    log (logging.Logger) - logger for this class

  Attributes:
  """

  log = logging.getLogger(os.path.basename(sys.argv[0])
                          + ".Service"
                          + ".DeviceMapper")

######################################################################
# Public methods
######################################################################
  @classmethod
  def deviceServiceType(cls, deviceName):
    """Returns the service type for the named device.
    """
    return cls(deviceName).serviceType()

  ####################################################################
  def configure(self, **kwargs):
    """Configures the device.
    """
    self._checkConfigureParameters()
    configureCommands = self._configureCommands(**kwargs)
    for command in configureCommands:
      cmd = Command(command.split())
      cmd.run()

  ####################################################################
  def create(self):
    """Creates the device.
    """
    self._checkCreateParameters()
    cmd = Command(self._createCommand(True))
    cmd.run()

  ####################################################################
  def disable(self):
    """Disables the device.
    """
    self._checkDisableParameters()
    self._disable()

  ####################################################################
  def enable(self):
    """Enables the device.
    """
    self._checkEnableParameters()
    self._enable()

  ####################################################################
  def remove(self):
    """Removes the device.
    """
    self._checkRemoveParameters()
    cmd = Command(self._removeCommand().split())
    cmd.run()

  ####################################################################
  def status(self):
    """Returns the status of the device.
    """
    self._checkStatusParameters()
    return self._serviceStatus()

  ####################################################################
  def serviceType(self):
    """Queries the device and returns its service type.
    """
    return self._serviceStatus().split()[2].strip()

######################################################################
# Overridden methods
######################################################################
  def __init__(self,
               deviceName,
               moduleName         = None,
               storageDevicePath  = None,
               **kwargs):
    super(DeviceMapper, self).__init__(self.__normalizedDeviceName(deviceName),
                                       **kwargs)
    self.__moduleName = moduleName
    self.__storageDevicePath = storageDevicePath
    self.__storageDeviceSize = None

######################################################################
# Protected methods
######################################################################
  def _checkCommonParameters(self):
    """Checks that the parameters common to all operations have been
       provided.

       Exceptions:
        DeviceMapperMissingParameter - required parameter missing
    """
    if self._deviceName() is None:
      raise DeviceMapperMissingParameter(_("missing device name"))

    if self._moduleName() is None:
      raise DeviceMapperMissingParameter(_("missing module name"))

  ####################################################################
  def _checkConfigureParameters(self):
    """Checks that the parameters necessary for configure have been provided.

       Exceptions:
        DeviceMapperMissingParameter - required parameter missing
    """
    self._checkCommonParameters()

  ####################################################################
  def _checkCreateParameters(self):
    """Checks that the parameters necessary for create have been provided.

       Exceptions:
        DeviceMapperMissingParameter - required parameter missing
    """
    if self._storageDevicePath() is None:
      raise DeviceMapperMissingParameter(_("require storage device path"))
    self._checkCommonParameters()

  ####################################################################
  def _checkDisableParameters(self):
    """Checks that the parameters necessary for disable have been provided.

       Exceptions:
        DeviceMapperMissingParameter - required parameter missing
    """
    self._checkCommonParameters()

  ####################################################################
  def _checkEnableParameters(self):
    """Checks that the parameters necessary for enable have been provided.

       Exceptions:
        DeviceMapperMissingParameter - required parameter missing
    """
    self._checkCommonParameters()

  ####################################################################
  def _checkRemoveParameters(self):
    """Checks that the parameters necessary for remove have been provided.

       Exceptions:
        DeviceMapperMissingParameter - required parameter missing
    """
    self._checkCommonParameters()

  ####################################################################
  def _checkStatusParameters(self):
    """Checks that the parameters necessary for status have been provided.

       Exceptions:
        DeviceMapperMissingParameter - required parameter missing
    """
    self._checkCommonParameters()

  ####################################################################
  def _configureCommands(self, **kwargs):
    """Returns the commands which will configure the device.
    """
    messages = self._configureMessages(**kwargs)
    return ["dmsetup message {device} 0 {msg}".format(
                  device = self._deviceName(), msg = message)
            for message in messages]

  ####################################################################
  def _configureMessages(self, **kwargs):
    """Returns the configuration messages to send to the device.
    """
    return []

  ####################################################################
  def _createCommand(self, forCommand = False):
    """Returns the command which will create the device.

       Arguments:
        forCommand (boolean)  if true, the command is returned as a list of
                              strings suitable to be passed to the Command
                              constructor.
    """
    command = ["dmsetup", "create", self._deviceName(), "--table",
               ("{table}" if forCommand else '"{table}"').format(
                table = self._makeTableString())]
    if not forCommand:
      command = " ".join(command)
    return command

  ####################################################################
  def _deviceName(self):
    """The device which this instance represents.
    """
    return self.getName()

  ####################################################################
  def _deviceType(self):
    """The type of device to create.
    """
    module = self._moduleName()
    match = re.match('^pbit(.*)', module)
    if match is None:
      raise RuntimeError(_("module {module} is not a Permabit module")
                          .format(module = module))
    module = match.group(1)
    return module

  ####################################################################
  def _disableCommand(self):
    """Returns the command which will disable the device.
    """
    return "dmsetup message {device} 0 disable".format(
            device = self._deviceName())

  ####################################################################
  def _dmTable(self):
    """The device mapper table.
    """
    try:
      status = self._serviceStatus()
    except CommandError as ex:
      status = ""

    table = " ".join(status.split()[0:5])
    if table == "":
      table = None
    return table

  ####################################################################
  def _disable(self):
    cmd = Command(self._disableCommand().split())
    cmd.run()

  ####################################################################
  def _enable(self):
    cmd = Command(self._enableCommand().split())
    cmd.run()

  ####################################################################
  def _enableCommand(self):
    """Returns the command which will enable the device.
    """
    return "dmsetup message {device} 0 enable".format(
            device = self._deviceName())

  ####################################################################
  def _makeTableString(self, resolvePath = False):
    """Make the dmsetup table to create the specified device.

       The assumption is that the device being created is 1:1 with the
       underlying device.  If this is not the case a subclass must override
       this method.

       Arguments:
        resolvePath (boolean)   if true, the table will contain the resolved
                                path
    """
    tableString = self._dmTable()
    if tableString is None:
      path = self._storageDevicePath()
      if path is not None:
        tableString = "0 {size} {type} {name} {path}".format(
                                              size = self._storageDeviceSize(),
                                              type = self._deviceType(),
                                              name = self._deviceName(),
                                              path = path)

    # If we have the table string, resolve the path of the device as specified.
    if (tableString is not None) and resolvePath:
      tableString = re.sub(r"(.*?)((/[A-Za-z0-9_:.-]+)+)",
                           lambda match : "{0}{1}".format(
                                    match.group(1),
                                    self.__resolveSymlink(match.group(2))),
                           tableString,
                           count = 1)

    return tableString

  ####################################################################
  def _mapStatusToControlMessage(self):
    """Gets the device status, decomposes it and returns a string of
       appropriate control messages to place the device in that state.
    """
    status = self._serviceStatus()

    # Right split on the table with the path resolved.
    (before, sep, remaining) = status.rpartition(self._makeTableString(True))
    remaining = remaining.strip()

    return (self._enableCommand() if "on" in remaining else
            self._disableCommand())

  ####################################################################
  def _modprobeCommand(self):
    """Return the command to insert the module in to the kernel.
    """
    return "modprobe {module}".format(module = self._moduleName())

  ####################################################################
  def _moduleName(self):
    """The name of the module providing the device implementation.
    """
    return self.__moduleName

  ####################################################################
  def _removeCommand(self):
    """Returns the command that will remove the device.
    """
    # Include --retry in case a transient udev process has the device open.
    return "dmsetup remove --retry {devName}".format(
                                                devName = self._deviceName())

  ####################################################################
  def _serviceName(self):
    """Returns the service name.
    """
    return self._deviceName()

  ####################################################################
  def _serviceStatus(self):
    """Returns the status of the service.
    """
    cmd = Command(self._statusCommand().split())
    return cmd.run().strip()

  ####################################################################
  def _startCommand(self, queryDevice = False):
    """Returns the command line necessary to start the device and, if
       querying of the device is specified, to set it to its current state.

       Arguments:
        queryDevice (bool)  - if true, the command(s) necessary to place the
                              device in its current state are included in the
                              returned command line.
    """
    command = self._createCommand()
    if queryDevice:
      stateCommand = self._mapStatusToControlMessage()
      if stateCommand is not None:
        command += ";" + stateCommand
    return command

  ####################################################################
  def _statusCommand(self):
    """Returns the command which will cause the device to report its status.
    """
    return "dmsetup status {devName}".format(devName = self._deviceName())

  ####################################################################
  def _stopCommand(self):
    """Returns the command that will stop the device.
    """
    return self._removeCommand()

  ####################################################################
  def _storageDevicePath(self):
    """Path to storage device to build upon.
    """
    return self.__storageDevicePath

  ####################################################################
  def _storageDeviceSize(self):
    """Size of the storage device to build upon in sectors.
    """
    if self.__storageDeviceSize is None:
      cmd = Command(["cat", self.__sysDevBlockPath("size")])
      self.__storageDeviceSize = ((long if sys.version_info < (3,0)
                                        else int)(cmd.run().strip()))
    return self.__storageDeviceSize

######################################################################
# Private methods
######################################################################
  @classmethod
  def __normalizedDeviceName(cls, deviceName):
    """Normalizes the device name for use in creating device mapper instances.

       Arguments:
        deviceName (string)   device name to normalize
    """
    return re.sub(r'/', '-', deviceName)

  ####################################################################
  def __deviceMajorMinor(self):
    """Returns the major/minor device numbers as a tuple for the storage
      device.
    """
    cmd = Command(["ls", "-Hl", self._storageDevicePath()])
    output = cmd.run().strip()
    matches = re.match('^b[rwx-]+T?\s+\d+[\s\w]+\s+(\d+),\s+(\d+)', output)
    if matches is None:
      raise RuntimeError(_("did not find major/minor for {device}").format(
                          device = self._storageDevicePath()))
    return matches.group(1, 2)

  ####################################################################
  def __resolveSymlink(self, path):
    """Fully resolve a possible symlink. If the device is not a symlink
       return the original path.

       Arguments:
        path (string)   path to resolve
    """
    resolvedPath = path
    if not os.access(resolvedPath, os.F_OK):
      raise DeviceMapperInvalidParameter(
              _("{path} does not exist").format(path = resolvedPath))
    if stat.S_ISLNK(os.lstat(resolvedPath).st_mode):
      cmd = Command(["readlink", "-f", resolvedPath])
      resolvedPath = cmd.run().strip()
    if resolvedPath == "":
      raise DeviceMapperInvalidParameter(
              _("{path} could not be resolved").format(path = path))
    return resolvedPath

  ####################################################################
  def __sysDevBlockPath(self, fileName):
    """Returns the pathname of a /sys/dev/block file associated with the
       storage device.

       Arguments:
        fileName (string)   file name for which to get path
    """
    (major, minor) = self.__deviceMajorMinor();
    return "/sys/dev/block/{major}:{minor}/{fileName}".format(
            major = major, minor = minor, fileName = fileName)

  ####################################################################
  def __withoutInfrastructureDependencies(self, dependencies):
    """Returns a new depdencies string with any infrastructure added
       dependencies removed.

       Infrastructure dependencies are defined as those which begin with '$'.
    """
    stripped = re.sub(r"\$[A-Za-z_]+", lambda match : "", dependencies).strip()
    return re.sub(r"\s+", " ", stripped)
