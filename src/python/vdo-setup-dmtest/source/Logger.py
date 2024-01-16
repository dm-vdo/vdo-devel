#!/usr/bin/env python3
"""
  Logger - Sets up the vdo_setup_dmtest logger

  Copyright (c) 2024 Red Hat
"""

from __future__ import print_function

import logging
from logging.handlers import RotatingFileHandler
import os

logger = logging.getLogger('vdo-setup-dmtest')
logger.setLevel(logging.DEBUG)

from .Utils import runCommand

class CustomFormatter(logging.Formatter):
  yellow   = "\x1b[38;5;226m"
  red      = "\x1b[38;5;196m"
  bold_red = "\x1b[31;1m"
  reset    = "\x1b[0m"

  def __init__(self, fmt):
    super().__init__()
    self.fmt = fmt
    self.FORMATS = {
      logging.DEBUG: self.fmt,
      logging.INFO: self.fmt,
      logging.WARNING: self.yellow + self.fmt + self.reset,
      logging.ERROR: self.red + self.fmt + self.reset,
      logging.CRITICAL: self.bold_red + self.fmt + self.reset
    }

  def format(self, record):
    log_fmt = self.FORMATS.get(record.levelno)
    formatter = logging.Formatter(log_fmt)
    return formatter.format(record)

def cleanLogs(logPath, numDays):
  """
  Clean log files older than the specified number of days
  """
  splitPath = os.path.split(logPath)
  logDir = splitPath[0]
  logName = '"' + splitPath[1] +  '*"'

  logger.debug("Cleaning old logs in '{loc}' with name {name}".format(loc = logDir, name = logName))

  return runCommand(['find', logDir, '-maxdepth 1', '-type f', '-name', logName,
                     '-mtime', '+' + str(numDays), '-delete'], logger).returncode

def configureLogger(file, debug=False):
  """
  Configure logging to the run log and console
  """
  debugging      = debug
  logFile        = file
  formatConsole  = '%(message)s'
  formatFile     = '%(asctime)s - ' + '%(levelname)s: %(message)s'

  # Configure logging to the console
  console_handler = logging.StreamHandler()
  console_handler.setLevel(logging.INFO)
  console_handler.setFormatter(CustomFormatter(formatConsole))
  logger.addHandler(console_handler)

  # Configure logging in the run log
  file_handler = RotatingFileHandler(logFile, maxBytes=10*1024*1024, backupCount=3)
  file_handler.setFormatter(CustomFormatter(formatFile))
  file_handler.setLevel(logging.DEBUG if debugging else logging.INFO)
  logger.addHandler(file_handler)
