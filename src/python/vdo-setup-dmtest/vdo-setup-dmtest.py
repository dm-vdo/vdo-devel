#! /usr/bin/env python3
"""
  Installs and sets up a dmtest-python environment for running vdo tests.

  Copyright (c) 2024 Red Hat
"""

from __future__ import print_function

import argparse
import git
import logging
import os
import stat
import subprocess
import sys
import yaml

from source.Logger import *
from source.Utils import *

RUN_DIR = os.getcwd()

class vdoDmtest(object):
  # Initialize variables that will be used throughout the script
  config           = {} # Dictionary of configuration definitions in config.yaml
  dmtestHealthWarn = [] # List of messages that should be displayed after `dmtest health` is run
  envVars          = [] # List of environment variables that need to be set/updated
  repoKeys         = [] # List of repo keys defined in the configuration file
  

  def __init__(self, args):
    # Parse input arguments
    (options, extra) = self.parseArgs(args)
    self.configFile  = options.configFile
    self.debug       = options.debug
    self.returnEnv   = options.returnEnv
    self.logFile     = options.logFile
    self.cleanLog    = options.numDays

    # Initialize the logging object
    self.log = logging.getLogger('vdo-setup-dmtest')
    configureLogger(self.logFile, self.debug)
    self.log.info("Starting vdo-setup-dmtest!")

    if (self.cleanLog > 0):
      cleanLogs(self.logFile, self.cleanLog)

    # Check for sudo priviledges
    if os.geteuid() != 0 and runCommandIgnoringErrors("sudo -v").returncode != 0:
      msg = "This script must be run by a user with sudo priviledges"
      raise PermissionError(msg)

    # Get the configuration
    self.getConfig(configFile = self.configFile)
    self.log.info("Configuration file read and processed")

    # Set workspace parameter if not defined in the configuration
    if not self.config['WORKSPACE']:
      self.config['WORKSPACE'] = os.path.join(RUN_DIR, 'repos')

    # Find all repo keys in the config object and store them in the self.repoKeys list
    self.getRepoKeys()
    self.log.debug("Repo keys found: {0}".format(self.repoKeys))

    # Verify the configuration device parameters
    self.verifyDevices()

    # Verify self.config['LINUX_SOURCE'] exists and is defined
    self.verifyLinuxSource()

  def parseArgs(self, args):
    """
    Parse any input arguments and apply defaults
    """
    parser = argparse.ArgumentParser(
      prog="vdo-setup-dmtest",
      description="Installs and sets up a dmtest-python environment for running vdo tests.",
      epilog="For additional information see README.md within the module directory.")
    parser.add_argument(
      "--config",
      action = "store",
      dest = "configFile",
      default = os.path.join(RUN_DIR,'config.yaml'),
      help = "The full path to the configuration YAML file. Default is './config.yaml'.")
    parser.add_argument(
      "--debug",
      action = "store_true",
      dest = "debug",
      default = False,
      help = "Turns on debug logging to the log file.")
    parser.add_argument(
      "--returnEnv",
      action = "store_true",
      dest = "returnEnv",
      default = False,
      help = "Turns on returning a list of environment variables that need to be exported" \
             + " upon successful completion.")
    parser.add_argument(
      "--cleanLog",
      action = "store",
      type = int,
      dest = "numDays",
      default = 7,
      help = "The number of days before recycling the log file. Default is 7.")
    parser.add_argument(
      "--runlog",
      action = "store",
      dest = "logFile",
      default = os.path.join(RUN_DIR, 'run.log'),
      help = "The full path to the log file for logging output. Default is './run.log'.")
    return parser.parse_known_args(args)

  def getConfig(self, configFile):
    """
    Read the config file and store the name value pairs in a dictionary
    """
    self.log.debug("Reading the config file at {0}".format(configFile))
    if not os.path.exists(configFile):
      self.log.error("Unable to read config file at {0}: No such file".format(configFile))
      sys.exit(1)

    with open(configFile, 'r') as file:
      loadedConfig = yaml.safe_load(file)

    if not bool(loadedConfig):
      self.log.error("Unable to load config file ({0})".format(configFile))
      return False

    self.config = loadedConfig

  def getRepoKeys(self):
    """
    Find all configuration keys corresponding to repositories and store them in a list
    """
    self.log.debug("Searching config for repo keys")
    for key, val in self.config.items():
      if key.upper().endswith('_REPO'):
        try:
          # Verify that the repository key found is defined
          assert val, " ".join([key, "is undefined"])
        except Exception as error:
          self.log.error("Error identified while processing the repo keys: {0}".format(error))
          sys.exit(1)
        self.repoKeys.append(key)
    self.repoKeys.sort()

  def verifyDevices(self):
    """
    Verify that the device configuration parameters exist and correspond to block devices
    """
    for key in ('DATA_DEVICE', 'METADATA_DEVICE'):
      self.log.debug("Searching config for device key '{0}'".format(key))
      try:
        # Verify key exists
        assert key in self.config.keys(), "not found"
        # Verify key is defined
        assert self.config[key], "is undefined"
        # Verify the value corresponds to a block device
        device = self.config[key]
        assert os.path.exists(device), "is defined with a path that does not exist"
        mode = os.stat(device).st_mode
        assert stat.S_ISBLK(mode), "does not correspond to a valid block device"
      except Exception as error:
        self.log.error("Required configuration key '{key}' {err}".format(key = key, err = error))
        sys.exit(1)
      self.log.debug("Key '{0}' found and verified".format(key))

  def verifyLinuxSource(self):
    """
    Verify that the linux source configuration parameter exists and is defined
    """
    key = 'LINUX_SOURCE'
    self.log.debug("Searching config for linux source key '{0}'".format(key))
    try:
        # Verify key exists
        assert key in self.config.keys(), "not found"
        # Verify key is defined
        assert self.config[key], "is undefined"
    except Exception as error:
      self.log.error("Required configuration key '{key}' {err}".format(key = key, err = error))
      sys.exit(1)
    self.log.debug("Key '{0}' found and verified".format(key))

  def setupRepos(self):
    """
    Create local clones of repos identified in the config file and run installation functions
    """
    localWorkspace = self.config['WORKSPACE']
    self.log.info("Local workspace directory set to '{0}'".format(localWorkspace))
    os.makedirs(localWorkspace, exist_ok=True)

    self.log.info("\nCloning the remote repositories defined in the config file")
    for key in self.repoKeys:
      self.log.debug("Processing key {0}".format(key))
      if (isURL(self.config[key])):
        # The key value is a URL, so proceed with cloning the repo or updating the local copy
        repoURL = self.config[key]

        # Clone or update the repo
        repoName = repoURL.rpartition("/")[-1].rsplit(".", 1)[0]
        repoPath = os.path.join(localWorkspace, repoName)
        if os.path.exists(os.path.join(repoPath)):
          self.log.info("Repo {0} already exists - updating".format(repoName))
          repo = git.Repo(repoPath)
          try:
            repo.remotes.origin.pull()
          except Exception as error:
            self.log.error("Unable to update the {name} repo: {reason}".format(name = repoName,
                                                                               reason = error))
            sys.exit(1)
        else:
          try:
              git.Repo.clone_from(repoURL, repoPath)
          except Exception as error:
            self.log.error("Unable to clone the {name} repo at {url}: {reason}".format(
              name = repoName, url = repoURL, reason = error))
            sys.exit(1)

          self.log.info("Repo {0} cloned successfully".format(repoName))

        # Add the repo path to the config structure
        self.config["_".join([repoName.upper(), 'PATH'])] = repoPath
        self.log.debug("Repo path is: {0}".format(repoPath))
      elif os.path.exists(self.config[key]):
        # The key value is a valid path, so proceed accordingly
        repoPath = self.config[key]

        # Add the repo path to the config structure
        repoName = repoPath.rpartition("/")[-1]
        self.config["_".join([repoName.upper(), 'PATH'])] = repoPath
        self.log.debug("Repo path is: {0}".format(repoPath))
      else:
        # The key value is invalid
        error = "Invalid definition"
        self.log.error("Failed to process repo key {key}: {reason}".format(key = key,
                                                                           reason = error))
        sys.exit(1)

  def installDeps(self):
    """
    Install package dependencies
    """
    self.log.info("Installing package dependencies listed in config file");
    if (self.config['PACKAGES']):
      try:
        result = runCommand(['sudo dnf install -y', " ".join(self.config['PACKAGES'])])
        assert result.returncode == 0, "Non-zero return status received"
      except Exception as error:
        self.log.error("Failed to install the required dependencies: {0}".format(error))
        sys.exit(1)
    else:
      self.log.info("Skipping - no package dependencies to install");

  def runRepoInstallSteps(self):
    """
    Run the correpsonding install method for each repository that was cloned
    """
    self.log.info("\nRunning the install methods for each repo")
    for key in self.repoKeys:
      repoName = self.config[key].rpartition("/")[-1].rsplit(".", 1)[0]
      repoPath = self.config["_".join([repoName.upper(), 'PATH'])]

      # Perform necessary installation steps for the repos
      self.log.info("Installing {0}".format(repoName))
      failureDetected = False
      try:
        os.chdir(repoPath)

        # The repo keys are sorted, so install should go in this order
        if (key.upper().find('BUFIO') != -1):
          self.installBufioTest(git.Repo(repoPath))
        elif (key.upper().find('BLK_ARCHIVE') != -1):
          self.installBlkArchive(repoName)
        elif (key.upper().find('DMTEST') != -1):
          self.installDmtestPython(repoName)
        else:
          msg = 'does not match any of the expected substrings (BUFIO | BLK_ARCHIVE | DMTEST)'
          self.log.warning("Repo key '{key}' {error} - Skipping".format(key = key, error = msg))
      except Exception as error:
        self.log.error("Failed during {name} installation: {reason}".format(name = repoName,
                                                                            reason = error))

        # If dmtest-python install failed, do not continue any further
        if (key.upper().find('DMTEST') != -1):
          sys.exit(1)
        else:
          failureDetected = True
          self.log.warning("Manual install of {name} in {path} required".format(name = repoName,
                                                                                path = repoPath))
      finally:
        os.chdir(RUN_DIR)

      msg = ''
      if not failureDetected:
        msg = "Completed installation of {0}\n".format(repoName)

      self.log.info("{0}".format(msg))

  def installBufioTest(self, repo):
    """
    Perform installation steps for the test-bufio repo
    """
    # Skip if module is already installed
    moduleName = 'dm_bufio_test'
    result = runCommandIgnoringErrors(['lsmod | grep', moduleName])
    if (result.returncode == 0):
      self.log.debug("Module {0} is already installed - skipping".format(moduleName))
      return

    # Obtain the list of remote OS-specific branches
    # Bufio-test historically only has rhel-8 and rhel-9 branches
    remoteBranches = []
    for ref in repo.remotes.origin.refs:
      if ("rhel" in ref.name):
        remoteBranches.append(ref.name.replace('origin/', "", 1))
    self.log.debug("Bufio remote OS-specific branches are: {0}".format(remoteBranches))

    # Determine the rhel release branch to use for installation
    if (os.path.exists('/etc/fedora-release')):
      osVersion = 9
    elif (os.path.exists('/etc/redhat-release')):
      result = runCommand('cut -d " " -f6 /etc/redhat-release | cut -d"." -f1')
      osVersion = result.stdout.rstrip()
    elif (os.path.exists('/etc/centos-release')):
      result = runCommand('cut -d " " -f4 /etc/centos-release')
      osVersion = result.stdout.rstrip()
    else:
      raise OSError("Unsupported OS in use")

    if (int(osVersion) > 9):
      osVersion = 9

    branch = 'rhel-' + str(osVersion)
    assert branch in remoteBranches, " ".join(['Invalid remote branch', branch])

    # Build and install the module for the running kernel
    repo.git.switch(branch)

    # Check for a module signing certificate/key pair, or create one
    certsDir = os.path.join('/lib/modules', os.uname().release, 'build/certs')
    certPath = os.path.join(certsDir, 'signing_key.pem')

    self.log.info('Checking for signing certificate {0}'.format(certPath))
    certExists = os.path.exists(certPath)
    if (certExists):
      certValid = verifyCertificate(certPath)

    if not certExists or not certValid:
      # Check for a x509.genkey file in the certs directory, or create one
      genkeyPath = os.path.join(certsDir, 'x509.genkey')
      if not (os.path.exists(genkeyPath)):
        assert createX509Genkey(certsDir) == True, 'Unable to create x509.genkey'

      # Create the signing certificate/key pair
      assert createSigningCertKeyPair(certPath, genkeyPath) == True, \
        'Signing certificate/key pair creation failed'

    # Run the commands to build, sign, and install the module
    self.log.info("Building module {0}".format(moduleName))
    commands = ['make -C /lib/modules/$(uname -r)/build M=$PWD',
                'sudo make -C /lib/modules/$(uname -r)/build M=$PWD modules_install',
                'sudo modprobe {0}'.format(moduleName)]
    for cmd in commands:
      result = runCommand(cmd)
      assert result.returncode == 0, 'Non-zero return status'

  def installBlkArchive(self, repoName):
    """
    Perform installation steps for the blk-archive repo
    """
    # Skip if already installed
    userCargoPath = os.path.join(os.path.expanduser('~'), '.cargo/bin')
    localExePath = os.path.join(os.getcwd(), 'target/release', repoName)
    cargoExePath = os.path.join(userCargoPath, repoName)
    if isExecutable(localExePath) and isExecutable(cargoExePath):
      self.log.debug("{0} is already installed - skipping".format(repoName))
      if userCargoPath not in os.environ["PATH"]:
        self.log.warning("PATH must include '{path}' for {repo} to function properly".format(
          path = userCargoPath, repo = repoName))

        # Append strings to necessary lists for reporting downstream
        self.envVars.append('PATH=$PATH{pathsep}{path}'.format(pathsep = os.pathsep,
                                                               path = userCargoPath))
        self.dmtestHealthWarn.append('Dependency {name} {msg}'.format(name = repoName,
          msg = 'shows as missing until PATH is updated'))
      return

    # Build and install blk-archive
    commands = ['cargo build --release',
                'cargo install --path $PWD']
    for cmd in commands:
      result = runCommand(cmd)
      assert result.returncode == 0, 'Non-zero return status received'
    self.log.warning("PATH must be updated to include '{0}'".format(userCargoPath))

    # Append strings to necessary lists for reporting downstream
    self.envVars.append('PATH=$PATH{pathsep}{path}'.format(pathsep = os.pathsep,
                                                           path = userCargoPath))
    self.dmtestHealthWarn.append('Dependency {name} {msg}'.format(name = repoName,
      msg = 'shows as missing until PATH is updated'))

  def installDmtestPython(self, repoName):
    """
    Perform installation steps for the dmtest-python repo
    """
    # Verify required files exist in local repo
    requirementsPath = os.path.join(os.getcwd(), 'requirements.txt')
    testDepPath = os.path.join(os.getcwd(), 'test_dependencies.toml')
    assert os.path.exists(requirementsPath), " ".join(['No such file', requirementsPath])
    assert os.path.exists(testDepPath), " ".join(['No such file', testDepPath])

    # Install the dmtest-python requirements
    result = runCommandIgnoringErrors(['python3 -m pip install -r', requirementsPath])
    assert result.returncode == 0, 'Unexpected error during installtion of requirements.txt'

    # Set up the linux kernel source tree
    assert self.setupDmtestKernelSource(), "Failed during kernel source set up"

    # Create the config file needed for running dmtest-python tests
    assert self.createDmtestConfig(), "Failed to create config.toml"

    # Display `dmtest health` output regarding dependency status for the user
    self.log.info("Displaying `dmtest health` output")
    result = subprocess.run('./dmtest health', shell=True, text=True, capture_output=True)
    self.log.info("{0}\n".format(result.stdout.rstrip()))

    # Display warning messages defined in self.dmtestHealthWarn
    for msg in self.dmtestHealthWarn:
      self.log.warning(msg)

  def setupDmtestKernelSource(self):
    """
    Set up the kernel source tree for dmtest-python
    """
    self.log.info("Setting up the kernel source tree specified in the configuration")
    defaultRepoPath = os.path.join(os.getcwd(), 'linux')
    relativeRepoPath = os.path.join('.', os.path.split(self.config['WORKSPACE'])[1],
                                    os.path.split(os.getcwd())[1], 'linux')
    dmtestEnvVar = os.getenv("DMTEST_KERNEL_SOURCE")

    if os.path.exists(defaultRepoPath):
      self.log.info("Kernel source already exists in {0} - Skipping".format(relativeRepoPath))
    elif dmtestEnvVar is not None:
      self.log.info("Kernel source already linked as {0} - Skipping".format(dmtestEnvVar))
    else:
      key = 'LINUX_SOURCE'
      kernelSource = self.config[key]
      # LINUX_SOURCE value is a URL
      if isURL(kernelSource):
        self.log.info("Kernel source repo {0} identified".format(kernelSource))

        # Clone the linux kernel source repo
        branchKey = key + '_BRANCH'
        if branchKey in self.config.keys():
          branch = self.config[branchKey]
          if branch is not None:
            self.log.info("Branch {0} will be cloned for the kernel source repo".format(branch))

        self.log.info("Cloning the kernel source repo into {0}".format(relativeRepoPath))
        errormsg = "Unable to clone the kernel source repo"
        if 'branch' in locals():
          assert git.Repo.clone_from(kernelSource, defaultRepoPath, branch=branch), errormsg
        else:
          assert git.Repo.clone_from(kernelSource, defaultRepoPath), errormsg
      # LINUX_SOURCE value is a valid full path
      elif os.path.exists(kernelSource):
        self.log.info("Kernel source directory '{0}' identified".format(kernelSource))
        self.log.warning("Environment variable {var} {msg}".format(var = 'DMTEST_KERNEL_SOURCE',
          msg = 'must be defined for dmtest to function properly'))

        # Append strings to necessary lists for reporting downstream
        self.envVars.append('DMTEST_KERNEL_SOURCE={val}'.format(val = kernelSource))
        self.dmtestHealthWarn.append('Kernel source shows as missing until {env} is set'.format(
          env = 'DMTEST_KERNEL_SOURCE'))
      # LINUX_SOURCE value is invalid
      else:
        msg = "{err} in key {key}".format(err = "Invalid kernel source tree definition",
                                          key = 'LINUX_SOURCE')
        raise ValueError(msg)
    return True

  def createDmtestConfig(self):
    """
    Create the config.toml configuration necessary for the dmtest-python package
    """
    filename = 'config.toml'
    configFile = os.path.join(os.getcwd(), filename)

    self.log.info("Creating the {0} configuration file".format(filename))

    try:
      with open(configFile, 'w+') as fh:
        fh.write("metadata_dev = '%s'\ndata_dev = '%s'\n%s\n" % (self.config['METADATA_DEVICE'],
                                                                 self.config['DATA_DEVICE'],
                                                                 'disable_by_id_check = true'))
    except Exception as error:
      self.log.error("Error - {0}".format(error))
    return True

def main(args):
  vdodmtest = vdoDmtest(args)

  vdodmtest.installDeps()
  vdodmtest.setupRepos()
  vdodmtest.runRepoInstallSteps()

  # Display environment variables that need to be set/updated
  if len(vdodmtest.envVars) > 0:
    vdodmtest.log.warning("Environment variables to export:")
    for var in vdodmtest.envVars:
      vdodmtest.log.warning("{0}".format(var))

  # Return the list of environment variables when '--returnEnv' is specified
  if vdodmtest.returnEnv:
    sys.stdout.write("; ".join(vdodmtest.envVars))

  vdodmtest.log.info("vdo-setup-dmtest operations completed successfully!")
  sys.exit(0)

if __name__ == "__main__":
  main(sys.argv)
