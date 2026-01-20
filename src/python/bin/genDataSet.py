#!/usr/bin/python3

#
# %COPYRIGHT%
#
# %LICENSE%
#
# $Id$

"""
Utility to generate a directory tree with random contents.

This class is configured through the command line or a properties file.
The recognized properties are:

  gen.<type>.min
      The minimum size of files for a particular type.

  gen.<type>.max
      The maximum size of files for a particular type.

  gen.<type>.num
      The number of files of a particular type to generate.

  gen.<type>.numCoalescent
      Of gen.<type>.num files generated, gen.<type>.numCoalescent
      will be coalescent copies of the other `(num - numCoalescent)`
      files generated. Defaults to zero if missing.

For the above properties, the recognized <type> values are
'small', 'medium', 'large',  and 'huge'.  These terms
don't have any particular meaning (in other words, 'small'
isn't required to be smaller than 'medium', and the ranges
may overlap) but are only for readability.

Files may also be copied from a source directory; this is used for
testing with files in a special format (for example, backup
files). The relevant properties are:

  gen.copied.num
      The number of files to copy.

  gen.copied.sourceDir
      The directory to copy files from.

Files copied in this manner are selected randomly (without
deletions) from the set in the provided directory. It is an error
for the directory to contain no ordinary files.

Files can be generated such that they consist of a number of blocks of
uniform but arbitrary size and a uniform but arbitrary ability to be
compressed, configured as such by the blockSize and percentage parameters.
By default, the percentage parameter is zero, indicating no compressibility,
and the block size is 4096. At zero compressibility, the blockSize parameter
is moot. Each block is made up of a compressible string of all-ones, followed
by random data, such that the compressible data occupies the configured
percentage of the block, and the random data occupies the rest of the block.
In this way, the file can be broken into blocks, each of which is compressible
at the confirgured percentage or very near it.

  gen.compressible.percentage
      The amount of compressible data per block, in percentage

  gen.compressible.blockSize
      Block size into which the generated file is divided

These properties are also available:

  gen.dirs.num
      The total number of directories to generate

  gen.dirs.maxDepth
      The maximum depth of the directory tree to generate

  gen.fsync
      if 'True', fsync files

  gen.root.dir
      The root directory to use (rarely useful)

  gen.readable_filenames
      If 'True', generated filenames are 'readable'

  gen.truncate
      If 'True', truncate files and overwrite

  gen.name_prefix
      A string to prefix names of the files and directories created

  gen.report_interval
       How often to print cumulative statistics (in seconds)

The gen.root.dir property is not used when the genTree(File) method
is used to generate the directory tree or if it is acceptable to use
the current directory (as defined by `os.getcwd()`)

The generator operates by adding entries to the specified root
directory: Files in the numbers specified, or directories
containing those objects.
"""

######################################################################

from __future__ import with_statement
from optparse import OptionParser

import copy
import filecmp
import itertools
import logging
import math
import numpy
import os
import shutil
import subprocess
import sys
import time


######################################################################
# Constants

DEFAULT_LOG_LEVEL = logging.INFO

KB = 1024;
MB = KB * KB;
GB = MB * KB;

# The maximum size of a chunk read from or written to a file.
MAX_CHUNK_SIZE = 64 * KB


######################################################################
# Exception Classes

class Error(Exception):
  """
  Generic exception raised for all errors
  """
  pass


######################################################################
# Classes

class GenerateStats:
  """
  Data-set size, timing, and throughput statistics.
  """

  def __init__(self):
    """
    Initializate a batch of statistics and start timing the run.
    """
    self.startTime = time.time()
    # Time taken, in milliseconds
    self.duration  = None
    self.numBytes  = 0
    self.numFiles  = 0
    self.numDirs   = 0
    self.seed      = None
    self.fps       = None
    self.mbps      = None
    self.verified  = True

  def calculate(self):
    """
    Finish timing the run, then calculate time and rate statistics.
    """
    seconds = time.time() - self.startTime;
    self.duration = seconds * 1000;
    self.fps      = self.numFiles / seconds
    self.mbps     = self.numBytes / MB / seconds
    self.seed     = gen.randseed

  def __str__(self):
    return (("%(numFiles)d files; %(numBytes)d bytes"
             " at %(fps).2f files/sec; %(mbps).2f MB/sec") % self.__dict__)


######################################################################
# Internal Classes

class Config(object):
  """
  Config instances store all the configuration properties.

  Config allows the syntactic conceit of referring to the the config
  properties without using any quoting or method calls. Instead of
  `config.getInt("gen.large.num")` or `int(config['gen.large.num'])`,
  by using a global Config named `gen`, we can just reference
  `gen.large.num` directly. We can also easily reference all the
  'gen.large.*' properties together.

  The hassle of explicitly allocating the hierarchy is avoided by
  automatically allocating any unreferenced names as nested Configs
  (much like defaultdict). For this to work, all property values must
  be initialized before they are ever referenced. This initialization
  also provides the type of the property, so we can distinguish the
  gen.boolean=False from gen.string=False without requiring string
  quoting on command-line overrides of the properties.
  """

  def __getattribute__(self, name):
    """
    Overridden to construct nested Config instances when first referenced.
    """
    try:
      return super(Config, self).__getattribute__(name)
    except AttributeError:
      if name.startswith("_"):
        raise AttributeError()

    # Automatically populate the requested property
    newChild = Config()
    setattr(self, name, newChild)
    return newChild

  def __setattr__(self, name, value):
    """
    Overridden to parse strings when the current value is not a string.
    """
    oldValue = self.__dict__.get(name)
    # The type check and eval here let us convert "False" to False if the old
    # value is bool, but keep it a string if the old value is a string.
    if (not ((oldValue is None) or isinstance(value, type(oldValue)))):
      # XXX use safer ast.literal_eval() when we get to python 2.6 or later
      value = type(oldValue)(eval(value, {}, {}))
    return super(Config, self).__setattr__(name, value)

  def addAll(self, kvPairs):
    """
    Call addSetting() on each (key,value) pair in a list.
    """
    for (key, value) in kvPairs:
      self.addSetting(key, value)

  def addSetting(self, key, value):
    """
    Recursively set key=value in this Config tree.
    """
    if (key.count(".") == 0):
      # No dots, so just set name=value.
      setattr(self, key, value)
    else:
      # Strip off the leading "prefix." from the name and recurse.
      (prefix, subkey) = key.split(".", 1)
      getattr(self, prefix).addSetting(subkey, value)

  def dump(self, prefix=""):
    """
    Recursively generate all the key=value strings stored under this config.
    """
    for (key, value) in self.__dict__.items():
      # Ignore the infinite self-loop at the top level.
      if (self == value):
        continue
      # Don't dump non-public values.
      if key.startswith("_"):
        continue
      if (type(value) == Config):
        # Recursively generate the nested entries.
        for x in value.dump(prefix + key + "."):
          yield x
      else:
        yield ("%s%s=%s" % (prefix, key, value))


class RandomState(numpy.random.RandomState):
  """
  Simple extension of the numpy package's pseudo-random number generator.
  """

  def __init__(self, seed=None):
    """
    Overrides the numpy constructor to accept any int by splitting the seed
    into an array of unsigned 32 bit values.
    """
    if isinstance(seed, int):
      seed = numpy.array((seed,)).view(numpy.uint32)
    super(RandomState, self).__init__(seed)

  def choice(self, seq):
    """
    Choose a random element from a non-empty sequence.
    """
    return seq[self.randint(len(seq))]

  def randomSeed(self):
    """
    Choose a random 64-bit seed.
    """ 
    return int(self.randint(256, size=8).astype(numpy.int8).view(numpy.int64)[0])


class Node(object):
  """
  Base class for the in-memory representation of the random data set.
  """

  # Most node classes are not directories.
  isDir = False

  def __init__(self):
    self.parent = None
    self.depth  = 0
    self.path   = None

  def setParent(self, parent):
    self.parent = parent
    self.depth  = parent.depth + 1

  def setName(self, name):
    self.path = os.path.join(self.parent.path, gen.name_prefix + str(name))

  def nameNodes(self, namer):
    """
    Set the node name using the next value returned from the generator.
    """
    self.setName(next(namer))

  def isFile(self, path):
    """
    Check that a file exists and is a regular file.
    """
    if not os.path.exists(path):
      log.error(path + ": missing!")
      return False
    if not os.path.isfile(path):
      log.error(path + ": not a regular file!")
      return False
    return True


class DirNode(Node):
  """
  A node that represents a directory. A container for other nodes.
  """
  isDir  = True

  def __init__(self):
    """
    Initialize an empty, unparented directory.
    """
    super(DirNode, self).__init__()
    self.children = []

  def isRoot(self):
    """
    Return True if this directory is the root of the tree.
    """
    return (self.parent is None)

  def addNode(self, node):
    """
    Add a node to this directory, making this node its parent.
    """
    node.setParent(self)
    self.children.append(node)

  def measure(self, stats):
    """
    Recursively gather statistics on this directory node and all its children.
    """
    if not self.isRoot():
      stats.numDirs += 1
    for node in self.children:
      node.measure(stats)

  def nameNodes(self, namer):
    """
    Recursively set the name of this directory node and all its children.
    """
    if not self.isRoot():
      super(DirNode, self).nameNodes(namer)
    for node in self.children:
      node.nameNodes(namer)

  def write(self):
    """
    Recursively create and write the files in this directory tree.
    """
    if not self.isRoot():
      log.debug(self)
      os.mkdir(self.path)
    for node in self.children:
      node.write()
    if gen.fsync:
      fd = os.open(self.path, 0)
      os.fsync(fd)
      os.close(fd)

  def verify(self):
    """
    Recursively verify the contents of files in this directory tree.
    """
    log.debug(self)
    if not os.path.isdir(self.path):
      log.error(self.path + ": directory missing!")
      return False
    return all([node.verify() for node in self.children])

  def __str__(self):
    return ("dir  %s" % self.path)


class FileNode(Node):
  """
  A node representating a file with randomly-generated contents.
  """

  def __init__(self, rng, category):
    """
    Initialize a file node, picking a random file size and seed.
    """
    super(FileNode, self).__init__()
    self.fileSeed = rng.randomSeed()
    self.fileSize = rng.random_integers(category.min, category.max)

  def measure(self, stats):
    """
    Add this file to the tally of statistics.
    """
    stats.numFiles += 1;
    stats.numBytes += self.fileSize;

  def write(self):
    """
    Create and write the random contents of this file.
    """
    log.debug(self)
    with open(self.path, "wb", 0) as output:
      if gen.truncate:
        self.truncate(output)
      output.seek(0)
      for chunk in self.generateChunks():
        output.write(chunk)
      if gen.fsync:
        output.flush();
        os.fsync(output.fileno());

  def truncate(self, file):
    """
    Truncate this file to the random file size.
    """
    file.truncate(self.fileSize)
    if (self.fileSize > 0):
      file.seek(self.fileSize - 1)
      file.write('\0')

  def verify(self):
    """
    Verify that the file exists and has the correct size and contents.
    """
    log.debug(self)
    if not (self.isFile(self.path) and self.verifySize()):
      return False
    if self.verifyContents():
      return True
    log.error("Filefrag output:")
    subprocess.call(['filefrag', '-v', self.path])
    return False

  def verifySize(self):
    """
    Verify that the file size matches the expected size.
    """
    size = os.path.getsize(self.path)
    if (size != self.fileSize):
      log.error("%s is %d bytes; should be %d"
                % (self.path, size, self.fileSize))
      return False
    return True

  def verifyContents(self):
    """
    Verify that the contents of the file match the expected random contents.
    """
    with open(self.path, "rb", 0) as input:
      for chunk in self.generateChunks():
        if not self.verifyChunk(input, chunk):
          return False
    return True

  def verifyChunk(self, input, expected):
    """
    Verify that the next chunk read from the file matches the random chunk.
    """
    actual = input.read(len(expected))
    if (actual == expected):
      return True
    if (len(actual) != len(expected)):
      log.error("%s contains fewer bytes than stat indicated" % self.path)
      return False
    # Report the file offset where the first difference occurs.
    offset = input.tell() - len(actual)
    offset += len(os.path.commonprefix((actual, expected)))
    log.error("{path} corrupt at {offset}\nwanted {expected}\ngot    {actual}"
              .format(path=self.path, offset=offset,
                      expected=expected.encode('hex'),
                      actual=actual.encode('hex')));
    return False

  def generateChunks(self):
    rng = RandomState(self.fileSeed)
    maxChunkSize = MAX_CHUNK_SIZE
    rate = float(gen.compressible.percentage)
    if (rate > 0):
      maxChunkSize = int(gen.compressible.blockSize)
    remaining = self.fileSize
    while (remaining > 0):
      chunkSize = min(remaining, maxChunkSize)
      remaining -= chunkSize
      compressSize = int(math.floor(chunkSize * (rate / 100.0)))
      chunk = bytes([255] * compressSize) + rng.bytes(chunkSize - compressSize)
      yield chunk

  def __str__(self):
    return ("file %s" % self.path)


class CopyNode(Node):
  """
  A node representating a file copied from an existing file.
  """

  def __init__(self, sourcePath):
    """
    Initialize a file node, specifying the file to be copied.
    """
    super(CopyNode, self).__init__()
    self.sourcePath = sourcePath

  def measure(self, stats):
    """
    Add this file to the tally of statistics.
    """
    stats.numFiles += 1;
    stats.numBytes += os.path.getsize(self.sourcePath)

  def write(self):
    """
    Create the data set file by copying the source file.
    """
    log.debug(self)
    shutil.copyfile(self.sourcePath, self.path)

  def verify(self):
    """
    Verify that the file exists and has the same contents as the source file.
    """
    log.debug(self)
    return (self.isFile(self.sourcePath)
            and self.isFile(self.path)
            and filecmp.cmp(self.sourcePath, self.path, shallow=False))

  def __str__(self):
    return ("file %s copied from %s" % (self.path, self.sourcePath))


class RandomDirTree(object):
  """
  A randomly-generated tree of directories and files.
  """

  def __init__(self, rootDir, stats):
    """
    Construct a random tree of nodes in memory and tally its statistics.
    """
    self.rng       = RandomState(gen.randseed)
    self.root      = DirNode()
    self.root.path = rootDir
    self.dirNodes  = [self.root]

    if (gen.dirs.maxDepth < 0):
      raise Error("MaxDirDepth must be positive: " + gen.dirs.maxDepth)

    # Create a random tree of nodes.
    self._createNodes()

    # Assign names to all the nodes, constructing paths as we go.
    if gen.readable_filenames:
      # "name" files with integers starting at zero.
      self.root.nameNodes(itertools.count(0))
    else:
      raise Error("ugly filenames are not currently supported")

    # All the nodes are allocated and the file sizes known, so we
    # can tally it up for the stats.
    self.root.measure(stats)

  def write(self):
    """
    Create all the directories, generate and write the files in the data set.
    """
    log.info("Writing random tree at " + self.root.path)
    self.root.write()

  def verify(self):
    """
    Verify the contents of all the files in the tree.

    Returns True if the entire tree is valid. All validation failures (missing
    files, mismatched contents) will be logged.
    """
    log.info("Verifying tree at " + self.root.path)
    return self.root.verify()

  def _createNodes(self):
    """
    Create and attach the directory, file, and copy nodes to the tree.
    """
    # Build a random tree of directory nodes.
    for i in range(gen.dirs.num):
      self._attach(DirNode())
    # If subdirectories were created, don't put files in the root dir.
    if (len(self.dirNodes) > 1):
      self.dirNodes.pop(0)
    # Create the file nodes for each size category.
    for category in (gen.huge, gen.large, gen.medium, gen.small):
      self._createFileNodes(category)
    # Generate all file copy nodes.
    self._createCopyNodes()

  def _attach(self, node):
    """
    Attach a node to this tree by adding it to a random directory node.
    """
    parent = self.rng.choice(self.dirNodes)
    if node.isDir:
      # Prevent the tree from getting too deep.
      if ((parent.depth > 0) and (parent.depth >= gen.dirs.maxDepth)):
        parent = parent.parent
      self.dirNodes.append(node)
    parent.addNode(node)

  def _createFileNodes(self, category):
    """
    Create and attach all the file nodes in a size category.
    """
    numFiles = category.num
    numCoal = category.numCoalescent
    numCoalLegacy = category.coalescent

    if ((numCoal > numFiles) or (numCoalLegacy > numFiles)):
      raise Error("A coalescent file count is greater than the maximum")

    while (numFiles > 0):
      # Create the base file that may be cloned, and place it into the tree.
      fileNode = FileNode(self.rng, category)
      numFiles -= 1
      self._attach(fileNode)

      if (numCoal > 0):
        # Do Barry-style coalescence: random number of coalescent
        # copies (1 - 3) for each up to numCoal files, clamped to the
        # maximum number of coalescent copies requested.
        if (numFiles == numCoal):
          copies = numCoal
        else:
          copies = min(numCoal, self.rng.random_integers(1, 3))
        numCoal -= copies
      elif (numCoalLegacy > 0):
        # We still need to create coalescent files; generate the
        # random number of copies we want (1 to 3), make sure that
        # the copies won't exceed the remaining numFiles limit, and
        # then clone the FileNode the appropriate number of times.
        copies = self.rng.random_integers(1, 3)
        numCoalLegacy -= 1
      else:
        copies = 0

      # We're making copies of this file; insure that we don't exceed the
      # maximum number of files specified for this size range.
      copies = min(copies, numFiles)
      if (copies > 0):
        log.debug("cloning %d with %d left" % (copies, numFiles))
        numFiles -= copies
        for i in range(copies):
          self._attach(copy.copy(fileNode))

  def _fileTree(self, top):
    for dirpath, dirnames, fnames in os.walk(top):
      for fname in fnames:
        yield os.path.join(dirpath, fname)

  def _createCopyNodes(self):
    """
    Create and attach all the CopyNodes to the tree.
    """
    if not gen.copied.num:
      return
    # Get a list of regular files from gen.copied.sourceDir
    fileList = list(self._fileTree(gen.copied.sourceDir))
    fileList = [f for f in fileList if os.path.isfile(f)]
    if not fileList:
      raise Error("no files found in " + gen.copied.sourceDir)
    for i in range(gen.copied.num):
      # Pick a random file from gen.copied.sourceDir to copy.
      sourcePath = self.rng.choice(fileList)
      self._attach(CopyNode(sourcePath))


######################################################################
# Internal functions

def initConfig():
  """
  Set the default value of all the gen.* configuration properties.
  """
  global gen
  gen = Config()

  # Make a self-referential top-level name so we can say either gen.foo or
  # getattr(gen, "gen.foo") and get the same referent.
  gen.gen = gen

  # The following are the parameters for generating the tree. Their default
  # values are provided here, but they can be overridden by values provided in
  # a configuration file.

  ## Set up file types
  gen.huge.num = 0
  gen.huge.coalescent = 0
  gen.huge.numCoalescent = 0
  gen.huge.min = 100 * MB
  gen.huge.max = 500 * MB

  gen.large.num = 0
  gen.large.coalescent = 0
  gen.large.numCoalescent = 0
  gen.large.min = 10 * MB
  gen.large.max = 15 * MB

  gen.medium.num = 0
  gen.medium.coalescent = 0
  gen.medium.numCoalescent = 0
  gen.medium.min = 1 * KB
  gen.medium.max = 200 * KB

  gen.small.num = 0
  gen.small.coalescent = 0
  gen.small.numCoalescent = 0
  gen.small.min = 0 * KB
  gen.small.max = 1 * KB

  gen.copied.num = 0
  gen.copied.sourceDir = ""

  gen.dirs.num = 0
  gen.dirs.maxDepth = sys.maxsize

  # Default compression rate is no compression
  gen.compressible.percentage = 0
  gen.compressible.blockSize = 4096

  # Defaults to the directory from which this was invoked.
  gen.root.dir = os.getcwd()

  # Filenames can be 'readable', or taken from the complete
  # set of legal filenames (including unprintable chars).
  gen.readable_filenames = True

  # Fsync the file before closing it
  gen.fsync = False

  # Truncate the file before overwriting it
  gen.truncate = False

  # Set by command line argument or defaulted by initialization
  gen.randseed = None

  # prefix to prepend to the names of created files
  gen.name_prefix = ""
  gen.report_interval = None


def parseArgs():
  """
  Parse command-line arguments. Returns `(options, args)`.
  """
  desc = "FILE must be a Properties file containing gen.* properties. " \
         "Alternatively, properties may be specified with -Dgen.*=<value>. " \
         "It's an error if no gen.* properties are specified."
  parser = OptionParser("usage: %prog [options] [FILE] [seed]",
                        description=desc)
  parser.add_option("-v", "--verify", action="store_true", default=False,
                    help="verify the tree instead of generate it")
  parser.add_option("-r", "--report", action="store", metavar="interval",
                    help="report progress every <interval> minutes")
  parser.add_option("-o", "--stdout", action="store_true", default=False,
                    help="print logging to STDOUT in addition to a file")
  parser.add_option("-l", "--logDir", action="store", default=".",
                    help="place log in the given directory")
  parser.add_option("-s", "--seed", action="store", type="int",
                    help="use the given random seed")
  parser.add_option("-D", "--define", action="append",
                    dest="settings", default=[],
                    help="define a gen.* value")

  (options, args) = parser.parse_args()

  if (len(args) > 2):
    parser.error("too many arguments")
  if (len(args) > 1):
    options.seed = args[1]

  return (options, args)


def configureLogging(options):
  """
  Set up logging to logDir/log.generator and to stdout (if selected).
  """
  logFile = os.path.join(options.logDir, "log.generator")
  logging.basicConfig(filename=logFile, filemode="w", level=DEFAULT_LOG_LEVEL,
                      format="%(asctime)s %(levelname)-8s %(message)s")
  if options.stdout:
    console = logging.StreamHandler(sys.stdout)
    formatter = logging.Formatter("%(name)-10s: %(levelname)-5s %(message)s")
    console.setFormatter(formatter)
    logging.getLogger("").addHandler(console)
  global log
  log = logging.getLogger("genDataSet")


def genTree(verify=False, rootDir=None, createRoot=True):
  """
  Generate (or verify) a random tree in either the current directory or the
  'gen.root.dir' property specified in the configuration.
  """
  if (rootDir is None):
    rootDir = gen.root.dir
    createRoot = True
  if createRoot:
    if not os.path.isdir(rootDir):
      os.makedirs(rootDir)
  if not os.path.isdir(rootDir):
    raise Error("No such directory: " + rootDir)

  # Start timing and initialize statistics.
  stats = GenerateStats()

  # We create the structure of the directory tree in memory first.
  tree = RandomDirTree(rootDir, stats)

  if verify:
    stats.verified = tree.verify()
    if (stats.verified):
      log.info("Tree is OK.")
    else:
      log.error("Tree is corrupted!");
  else:
    tree.write()

  # Finish timing and calculate statistics.
  stats.calculate()
  return stats


def main():
  print("starting genDataSet (on stdout)", file=sys.stdout)
  print("starting genDataSet (on stderr)", file=sys.stderr)

  (options, args) = parseArgs()

  configureLogging(options)

  initConfig()
  gen.addAll(kv.split('=') for kv in options.settings)

  if args:
    log.info("Using Config file " + args[0])
    print("XXX config files are not yet implemented", file=sys.stderr)
    return 2

  # override gen.* settings with command-line switches
  if (options.seed is not None):
    gen.randseed = options.seed
  # If randseed is still not specified, generate a seed except in
  # verify mode
  if (gen.randseed is None):
    if options.verify:
      log.error("Random seed must be specified in verify mode.")
      return 2
    gen.randseed = RandomState().randomSeed()

  if (options.report is not None):
    gen.report_interval = options.report

  percent = int(gen.compressible.percentage)
  if ((percent < 0) or (percent > 100)):
    log.error("Compression percentage must be between 0 and 100.")
    return 2

  log.info("Starting genDataSet\n\t" + "\n\t".join(gen.dump("gen.")))
  stats = genTree(options.verify)
  log.info(("Verified " if options.verify else "Wrote ") + str(stats))
  return int(not stats.verified)


if (__name__ == "__main__"):
  sys.exit(main())
