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
class Mismatch(object):
  """Represents a mismatch discovered from the trace output.
  """
  #############################################################################
  def __init__(self, sector, expectedHash, actualHash, sectorsWithExpected,
               sectorsWithActual):
    """
    Initializer of the mistmatch instance.

    Parameters:
      [in]    sector                the sector with the mismatch
      [in]    expectedHash          the expected hash for the sector
      [in]    actualHash            the actual hash for the sector
      [in]    sectorsWithExpected   list of sectors that have the expected hash
      [in]    sectorsWithActual     list of sectors that have the actual hash
    """
    super(Mismatch, self).__init__()
    self.__sector = sector
    self.__expectedHash = expectedHash
    self.__actualHash = actualHash
    self.__sectorsWithExpected = sectorsWithExpected[:]
    self.__sectorsWithActual = sectorsWithActual[:]


  #############################################################################
  def __str__(self):
    return """{header1}{header2}
\tsectors with expected: {expectedSectors}"
\tsectors with found: {foundSectors}""".format(
    header1 = "sector {sector} hash mismatch".format(sector = self.sector),
    header2 = ", expected: {expected}, found: {found}".format(
                expected = self.expectedHash, found = self.actualHash),
    expectedSectors = self.sectorsWithExpected,
    foundSectors = self.sectorsWithActual)

  #############################################################################
  @property
  def actualHash(self):
    return self.__actualHash

  #############################################################################
  @property
  def expectedHash(self):
    return self.__expectedHash

  #############################################################################
  @property
  def sector(self):
    return self.__sector

  #############################################################################
  @property
  def sectorsWithActual(self):
    return self.__sectorsWithActual

  #############################################################################
  @property
  def sectorsWithExpected(self):
    return self.__sectorsWithExpected

###############################################################################
class TraceFile(object):
  # The operations that the tracer dm target logs.
  _discardOp = "D"
  _flushOp = "F"
  _readOp = "R"
  _writeOp = "W"

  # The operations we care to pass to the checking routine.
  _pertinentOperations = [_discardOp, _readOp, _writeOp]

  # The string identifying a tracer output line
  # The tracer name comes after this, but any tracer line has to have this in
  # it.
  _tracerLineId = "pbit-tracer, "

  # The pattern to use to perform the initial split of a tracer output line.
  _tracerLineSplitPattern = r"\[{id}.+\]".format(id = _tracerLineId)

  # The pattern to use to perform a split of a tracer output line when the
  # split using the above fails.
  _tracerPartialLineSplitPattern = r"\[{id}".format(id = _tracerLineId)

  # The max number of tuples to accumulate before processing.  This helps to
  # keep memory consumption down.
  _traceTupleLimit = 128

  # The length of the hash.
  _hashLength = 32

  #############################################################################
  def __init__(self, arguments):
    """
    Initializer of the trace file instance.

    Parameters:
      [in]    arguments   the arguments; debug, files, quiet
                          if quiet, processing exits on first mismatch
    """
    super(TraceFile, self).__init__()
    self.__arguments = arguments
    self.__sectorToHash = {}
    self.__hashToSectors = {}
    self.__mismatches = []

  #############################################################################
  def _addSectorInfo(self, sector, hash):
    """
    Adds the information for the specified sector.

    Parameters:
      [in]      sector    the sector to update
      [in]      hash      the hash associated with the sector
    """
    try:
      self._debugPrint("removing {sector} from hash-to-sector mapping"
                        .format(sector = sector))
      self.__hashToSectors[self._sectorHash(sector)].remove(sector)
    except KeyError:
      pass

    self._debugPrint(
      "adding/updating {sector}:{hash} in sector-to-hash mapping"
        .format(sector = sector, hash = hash))

    self.__sectorToHash[sector] = { "hash"    : hash,
                                    "single"  : True }

    self._debugPrint("adding {hash}:{sector} to hash-to-sector mapping"
                      .format(sector = sector, hash = hash))
    if hash not in self.__hashToSectors:
      self.__hashToSectors[hash] = []
    self.__hashToSectors[hash].append(sector)

  #############################################################################
  def _debugPrint(self, stringToPrint):
    """
    Prints the specified string to the target debug output.

    Parameters:
      [in]      stringToPrint string to print
    """
    print(stringToPrint, file = self.__arguments.debug)

  #############################################################################
  def _discardSectorInfo(self, sector):
    """
    Discards the information for the associated sector.

    Parameters:
      [in]      sector    the sector to discard
    """
    hash = None
    try:
      hash = self._sectorHash(sector)
    except KeyError:
      pass
    else:
      self._debugPrint("dropping {sector} info".format(sector = sector))
      del self.__sectorToHash[sector]
      self.__hashToSectors[hash].remove(sector)

  #############################################################################
  def _processMismatch(self, sector, hash):
    """
    Process the mismatch for the specified sector.

    Parameters:
      [in]      sector    the sector with mismatch
      [in]      hash      the hash associated with the sector
    """
    try:
      foundSectors = self.__hashToSectors[hash]
    except KeyError:
      foundSectors = []

    self.__mismatches.append(Mismatch(sector,
                                      self._sectorHash(sector),
                                      hash,
                                      sorted(self.__hashToSectors[
                                                    self._sectorHash(sector)]),
                                      sorted(foundSectors)))
    self._debugPrint(str(self.__mismatches[-1]))

  #############################################################################
  def _processTraceTuples(self, traceTuples):
    """
    Processes the specified array of trace tuples populating and checking the
    sector-to-hash and hash-to-sector dictionaries for hash mismatches.

    Parameters:
      [in]      traceTuples   array of trace tuples to process; the format of
                              each tuple is (operation, sector, hash).
    Returns:
      status code - if zero, success; else, error
    """
    result = 0

    for (op, sector, hash) in traceTuples:
      sector = int(sector)
      if op == self._discardOp:
        self._discardSectorInfo(sector)
      elif (op == self._writeOp) or (sector not in self.__sectorToHash):
        self._addSectorInfo(sector, hash)
      elif op == self._readOp:
        self.__sectorToHash[sector]["single"] = False
        if hash != self._sectorHash(sector):
          self._processMismatch(sector, hash)
          result = -1 if not self.__arguments.singles else 0
          if self.__arguments.quiet and (not self.__arguments.singles):
            break

    return result

  #############################################################################
  def _sectorHash(self, sector):
    """
    Returns the hash value being tracked for the specified sector.

    Throws exception if the sector does not exist in the tracking data.

    Parameters:
      [in]      sector    the sector with mismatch

    Returns:
      hash value (string)
    """
    return self.__sectorToHash[sector]["hash"]

  #############################################################################
  def mismatches(self):
    """
    Returns a copy of the found mismatches array.
    """
    return self.__mismatches[:]

  #############################################################################
  def process(self):
    """
    Processes the file with which the instance was initialized looking for
    mismatches.

    Returns:
      status code - if zero, success; else, error
    """
    savedResult = 0

    traceTuples = []

    # Iterate over the lines in the source file extracting those which come
    # from tracer and checking them for sector hash mismatches.
    #
    # We process the extracted lines piecemeal to keep memory consumption
    # relatively small.
    firstSplit = re.compile(self._tracerLineSplitPattern)
    for infile in self.__arguments.infile:
      for line in infile:
        line = line.strip()
        if self._tracerLineId not in line:
          continue

        hash = None
        try:
          (leftSide, hashSide) = firstSplit.split(line, 1)
        except ValueError:
          # Didn't get a complete tracer line.
          leftSide = re.split(self._tracerPartialLineSplitPattern, line, 1)[0]
          hashSide = ""

        hashSide = hashSide.strip()
        if hashSide != "":
          hash = hashSide.rsplit("hash:", 1)[-1].strip()
          if (len(hash) < self._hashLength):
            # Didn't get a complete tracer line; this one cutoff in the hash.
            hash = None

        (leftSide, _) = leftSide.rstrip().rsplit("+", 1)

        # Get the starting sector and the operation field.
        (leftSide, sector) = leftSide.rstrip().rsplit(" ", 1)
        operation = leftSide.rstrip().rsplit(" ", 1)[-1].strip()

        # Theoretically, the operation field could be a combination of
        # [F]<D|R|W|N>[F].  That is...
        # optional flush, discard/read/write/other, optional fua
        # For our purpose of checking for hash mismatches we only care about
        # actual discard/read/write.
        operation = operation[0]
        if operation in self._pertinentOperations:
          # If no hash and not a discard we have been dealing with an
          # incomplete tracer line.  If the operation is a write, make it a
          # discard.  If it's a read, ignore it.
          #
          if (hash is None) and (operation != self._discardOp):
            if (operation == self._readOp):
              print("dropping read due to incomplete line: {line}"
                      .format(line = line), file = sys.stderr)
              continue
            else:
              print("converting write to discard due to incomplete line: {line}"
                      .format(line = line), file = sys.stderr)
              operation = self._discardOp

          # Add the tuple to our accumulating list.
          traceTuples.append((operation, sector, hash))

          # If we hit the tuple accumulation limit process the tuples.
          if len(traceTuples) == self._traceTupleLimit:
            result = self._processTraceTuples(traceTuples)
            if (savedResult == 0) or (result != 0):
              savedResult = result
            if (savedResult != 0) and self.__arguments.quiet:
              break
            traceTuples = [];

      if (savedResult != 0) and self.__arguments.quiet:
        break

    if (savedResult == 0) or (not self.__arguments.quiet):
      # Process any tuples we accumulated but not enough to hit the
      # accumulation limit.
      result = self._processTraceTuples(traceTuples)
      if (savedResult == 0) or (result != 0):
        savedResult = result

    return savedResult

  #############################################################################
  def singles(self):
    """
    Returns an array containing all the sectors that were only seen a single
    time.
    """
    return [sector for sector in self.__sectorToHash
            if self.__sectorToHash[sector]["single"]]

###############################################################################
def main(arguments):
  traceFile = TraceFile(arguments)
  result = traceFile.process()

  if not arguments.singles:
    if not arguments.quiet:
      mismatches = traceFile.mismatches()
      print("Mismatches:{extra}".format(extra
                                      = "" if len(mismatches) > 0 else " []"),
            file = arguments.outfile)
      for mismatch in sorted(mismatches,
                             key = lambda mismatch : mismatch.sector):
        print("  - sector: {0}".format(mismatch.sector),
              file = arguments.outfile)
        print("    expected: {0}".format(mismatch.expectedHash),
              file = arguments.outfile),
        print("    actual: {0}".format(mismatch.actualHash),
              file = arguments.outfile),

        print("    sectors with expected hash:{extra}".format(extra
                    = "" if len(mismatch.sectorsWithExpected) > 0 else " []"),
              file = arguments.outfile)
        for sector in mismatch.sectorsWithExpected:
          print("      - {0}".format(sector), file = arguments.outfile)

        print("    sectors with actual hash:{extra}".format(extra
                      = "" if len(mismatch.sectorsWithActual) > 0 else " []"),
              file = arguments.outfile)
        for sector in mismatch.sectorsWithActual:
          print("      - {0}".format(sector), file = arguments.outfile)
  elif result == 0:
    singles = traceFile.singles()
    print("Single sectors:{extra}".format(extra
                                          = "" if len(singles) > 0 else " []"),
          file = arguments.outfile)
    for sector in sorted(singles):
      print("  - {0}".format(sector), file = arguments.outfile)

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
  parser.add_argument("-q", "--quiet",
                      action = "store_true",
                      default = False,
                      help = """suppress normal output; used for exit status;
                                if mismatch detected processing is aborted;
                                debug output, if specified, is not
                                suppressed""")
  parser.add_argument("-s", "--singles",
                      action = "store_true",
                      default = False,
                      help = """if specified, sectors that have been seen only
                                once are reported rather than detected
                                mismatches; discarded sectors have their state
                                reset""")

  sys.exit(main(parser.parse_args()))
