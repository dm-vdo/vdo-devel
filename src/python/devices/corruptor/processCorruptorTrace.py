#! /usr/bin/env python

#
# %COPYRIGHT%
#
# %LICENSE%
#

from __future__ import print_function

import argparse
import re
import sys

###############################################################################
class CorruptorTraceFile(object):
  # The operations that the corruptor dm target logs.
  _readOp = "CR"
  _writeOp = "CW"

  # The logged operations that corrupt data.
  _corruptSectorOp = [_readOp, _writeOp]

  # The string identifying a corruptor output line
  # The corruptor name comes after this, but any corruptor line has to have
  # this in it.
  _lineId = "pbit-corruptor, "

  # The pattern to use to perform the initial split of a corruptor output line.
  _lineSplitPattern = r"\[{id}.+\]".format(id = _lineId)

  # The max number of tuples to accumulate before processing.  This helps to
  # keep memory consumption down.
  _tupleLimit = 128

  #############################################################################
  def __init__(self, arguments):
    """
    Initializer of the trace file instance.

    Parameters:
      [in]    arguments   the arguments; debug, files, quiet
                          if quiet, processing exits on first mismatch
    """
    super(CorruptorTraceFile, self).__init__()
    self.__arguments = arguments
    self.__corruptions = set()

  #############################################################################
  def _debugPrint(self, stringToPrint):
    """
    Prints the specified string to the target debug output.

    Parameters:
      [in]      stringToPrint string to print
    """
    print(stringToPrint, file = self.__arguments.debug)

  #############################################################################
  def _processTuples(self, tuples):
    """
    Processes the specified array of tuples populating the corruption set.

    Parameters:
      [in]      Tuples      array of tuples to process; the format of
                            each tuple is
                              (operation, start sector, sector length).
    Returns:
      status code - if zero, success; else, error
    """
    result = 0

    for (op, startSector, length) in tuples:
      if op not in self._corruptSectorOp:
        continue

      startSector = int(startSector)
      length = int(length)
      self.__corruptions |= set([sector for sector
                                  in range(startSector, startSector + length)])

    return 0

  #############################################################################
  def printYaml(self):
    """
    Prints the set of corrupted sectors as YAML.
    """
    print("Corrupted sectors:{extra}"
            .format(extra = "" if len(self.__corruptions) > 0 else " []"),
          file = self.__arguments.outfile)
    for sector in sorted(self.__corruptions):
      print("  - {sector}".format(sector = sector),
            file = self.__arguments.outfile)

  #############################################################################
  def process(self):
    """
    Processes the file with which the instance was initialized looking for
    corruption instances.

    Returns:
      status code - if zero, success; else, error
    """
    result = 0

    tuples = []

    # Iterate over the lines in the source file extracting those which come
    # from corruptor and updating the set of corruptions.

    firstSplit = re.compile(self._lineSplitPattern)
    for infile in self.__arguments.infile:
      for line in infile:
        if self._lineId not in line:
          continue

        sectorLength = "1"
        (leftSide, rightSide) = firstSplit.split(line, 1)
        (leftSide, sectorLength) = leftSide.rstrip().rsplit("+", 1)
        sectorLength = sectorLength.strip()

        # Get the starting sector and the operation field.
        (leftSide, sector) = leftSide.rstrip().rsplit(" ", 1)
        operation = leftSide.rstrip().rsplit(" ", 1)[-1].strip()

        if operation in self._corruptSectorOp:
          self._debugPrint(
            "found corruption; sector: {sector}, length: {length}"
              .format(sector = sector, length = sectorLength))
          tuples.append((operation, sector, sectorLength))

          # If we hit the tuple accumulation limit process the tuples.
          if len(tuples) == self._tupleLimit:
            result = self._processTuples(tuples)
            if result != 0:
              break
            tuples = [];

      if result != 0:
        break

    if result == 0:
      # Process any tuples we accumulated but not enough to hit the
      # accumulation limit.
      result = self._processTuples(tuples)

    return result

###############################################################################
def main(arguments):
  traceFile = CorruptorTraceFile(arguments)
  result = traceFile.process()
  if result == 0:
    traceFile.printYaml()
  return result

###############################################################################
if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("-d", "--debug",
                      nargs = "?",
                      type = argparse.FileType("w"),
                      const = sys.stdout,
                      default = open("/dev/null", "w"),
                      help = """produce debug output; an optional value can be
                                provided with this option and is interpretted
                                as the file to which to write the debug output;
                                if no optional value is associaated the debug
                                output is written to stdout.""")
  parser.add_argument("infile",
                      nargs = "*",
                      type = argparse.FileType("r"),
                      default = [sys.stdin],
                      help = """file containing blkparse output; if more than
                                one file is specified they are processed, in
                                order, as an aggregate; defaults to stdin""")
  parser.add_argument("-o", "--outfile",
                      nargs = "?",
                      type = argparse.FileType("w"),
                      const = sys.stdout,
                      default = sys.stdout,
                      help = """file to which to write output;
                                defaults to stdout""")

  sys.exit(main(parser.parse_args()))
