/*
 * streamGen - a fio add-on that generates synthetic data defined by a
 * configuration file.
 *
 * Copyright (c) 2021 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <assert.h>
#include "/usr/include/err.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "streamGen.h"

#include "pst.h"

enum {
  MAX_STREAM_NAME = 32
};

typedef struct anyStream        AnyStream;
typedef struct anyStreamDefHead AnyStreamDefHead;
typedef struct runQueueHead     RunQueueHead;
TAILQ_HEAD(anyStreamDefHead, anyStreamDef);
STAILQ_HEAD(runQueueHead, anyStreamDefQueue);

typedef struct anyStreamDef AnyStreamDef;

typedef enum {
  STREAM_TAG_SIMPLE    = 1,
  STREAM_TAG_ALIAS     = 2,
  STREAM_TAG_SHUFFLED  = 3,
  STREAM_TAG_MIXED     = 4,
  STREAM_TAG_REPEATING = 5
} AnyStreamTag;

typedef enum {
  STREAM_SHUFFLING_XOR     = 1,
  STREAM_SHUFFLING_REVERSE = 2
} StreamShuffling;

typedef struct aliasStreamDef {
  const AnyStreamDef *substream;
} AliasStreamDef;

typedef struct repeatingStreamDef {
  const AnyStreamDef *substream;
  uint64_t numRepetitions;
} RepeatingStreamDef;

typedef struct shuffledStreamDef {
  StreamShuffling     shuffling;
  unsigned int        chaining;
  unsigned int        seed;
  const AnyStreamDef *substream;
} ShuffledStreamDef;

typedef enum {
  CHAIN_FIXED  = 1,
  CHAIN_FIXED_LENGTH = 2,
  CHAIN_RANDOM = 3
} ChainType;

typedef struct mixedStreamDef  {
  ChainType            chainType;
  uint64_t             mixing;
  unsigned int         seed;
  unsigned int         numSubstreams;
  const AnyStreamDef **substreams;
} MixedStreamDef;

struct anyStreamDef {
  TAILQ_ENTRY(anyStreamDef) links; // top-level list of all definitions
  char         *name;
  uint64_t      length;         // total length of the stream
  AnyStreamTag  tag;
  union {
    AliasStreamDef     alias;
    ShuffledStreamDef  shuffled;
    MixedStreamDef     mixed;
    RepeatingStreamDef repeating;
  } stream;
};

typedef struct anyStreamDefQueue {
  const AnyStreamDef *def;
  STAILQ_ENTRY(anyStreamDefQueue) link; // for run list
} AnyStreamDefQueue;

typedef struct simpleStream {
  uint64_t counter;
} SimpleStream;

typedef struct AliasStream {
  uint64_t   counter;
  AnyStream *substream;
} AliasStream;

typedef struct repeatingStream {
  uint64_t   numRepetitions;
  AnyStream *substream;
} RepeatingStream;

typedef struct shuffledStream {
  uint64_t           counterXor;
  uint64_t           highestBit;
  AnyStream         *substream;
} ShuffledStream;

typedef struct mixedStream {
  struct random_data   randData;
  char                 randState[8];
  uint64_t             chainRemainder;
  unsigned int         selected;
  PstNode             *substreams; // tree of substreams
  PstNode             *rootSubstream;
  uint64_t             remainingSubstreamsLength;
  unsigned int         remainingSubstreamsCount;
} MixedStream;

struct anyStream {
  const AnyStreamDef *definition;
  union {
    SimpleStream    simple;
    AliasStream     alias;
    ShuffledStream  shuffled;
    MixedStream     mixed;
    RepeatingStream repeating;
  } stream;
};

typedef struct albStreamInfo {
  AnyStreamDefHead streamDefs;
  RunQueueHead streamRuns;
  AnyStream *streamInst;
  uint64_t totalRunLength;
  char *dataBuffer;
} AlbStreamInfo;

static void setThreadSeed(const AnyStreamDefHead *streamDefs, int threadNumber);

static const unsigned int SEED_MOD_FACTOR = ('f' << 16) + ('i' << 8) + 'o';

static AlbStreamInfo* threadAlbStreamInfo = NULL;
static int maxThreads = 0;

/**
 * Language definition:
 *
 * # <comments>
 * # defines a simple stream where each chunk consists of the name (truncated
 * # to 16 bytes), then a monotonically increasing 64-bit counter, then all
 * # zeroes.
 * stream <name> simple { length = <number of chunks> }
 *
 * # defines a stream that aliases another stream and plays a subset (the
 * # supplied length) of its records (starting from the beginning).
 * stream <name> alias {
 *   substream = <stream to alias>
 *   length = <number of chunks>
 * }
 *
 * # defines a stream that consists of some basic shuffling of exactly one
 * # substream.  If the shuffling type is 'xor', the index of a chunk is xor'ed
 * # to a shuffler s, where s = <random number> * 2^(c+1) + c, meaning chains
 * # of 2^c will remain in sequential order.  If the shuffling type is
 * # 'reverse', the upper part offsets are bit-reversed (bit c and highest bit
 * # are swapped, etc. until the middle), to produce a long-jumping access
 * # pattern, modulo chains of length 2^c.  Note: the substream cannot be of a
 * # mixed type.
 * stream <name> shuffled {
 *   shuffling = (xor|reverse)
 *   chaining = <chain length exponent>
 *   seed = <random seed for shuffling>
 *   substream = <stream to shuffle>
 * }
 *
 * # produces a mix of substreams (minimum 2, can use the same one multiple
 * # times), using up all the data, i.e. with a length n equal to the sum of
 * # all substreams.
 * #  - With a 'fixed' chain type, the "mixing factor" is x such that the
 * #    chain length is l/x where l is the length of the currently selected
 * #    substream.
 * #  - With a 'fixed_length' chain type, the chain length is x chunks.
 * #  - With a 'random' chain type, the probability of switching streams at
 * #    any point is x/l.
 * # On a switch, a new substream is selected, with each substream having a
 * # probability of being selected of L/n, where L is the length of that
 * # substream. The new substream may be the same as the previous substream.
 * # If the number of substreams specified is less than numsubstreams, the
 * # substreams are repeated in order as needed.
 * stream <name> mixed {
 *   chaintype = (fixed|fixed_length|random)
 *   mixing = <mixing factor>
 *   seed = <random seed for mixing>
 *   numsubstreams = <number of substreams>
 *   substream = <stream to mix>
 *   substream = <other stream to mix>
 *   [substream = ... ]
 * }
 *
 * # A repeating stream repeats its substream repetitions times.
 * # Each repetition produces the same sequence of chunk names.
 * stream <name> repeating {
 *   substream = <stream to repeat>
 *   repetitions = <number of repetitions>
 * }
 *
 * # runs the program by feeding each named stream to the uds library, in
 * # order.
 * run { <stream name> [<stream name> ...] }
 **/

/**********************************************************************/
static const AnyStreamDef *lookupStreamDef(const AnyStreamDefHead *streamDefs,
                                           const char		   *name)
{
  AnyStreamDef *def;

  TAILQ_FOREACH_REVERSE(def, streamDefs, anyStreamDefHead, links) {
    if (strcmp(name, def->name) == 0) {
      return def;
    }
  }
  return NULL;
}

/**********************************************************************/
static void addStreamDef(AnyStreamDefHead *streamDefs,
                         AnyStreamDef     *def)
{
  TAILQ_INSERT_TAIL(streamDefs, def, links);
}

/**********************************************************************/
// returns true if EOF, exits if read error
static bool checkDone(int scanRet, FILE *file)
{
  if (scanRet == EOF) {
    if (ferror(file) != 0) {
      errx(1, "Error while parsing config file");
    }
    return true;
  }
  return false;
}

/**********************************************************************/
// returns multiplied length on success, exits on invalid multiplier
static uint64_t applyMultiplier(char multiplier, uint64_t length)
{
  // Due to the nature of the parsing, a closing brace may be read in place
  // of a multiplier suffix. In this case, it means that there is no
  // multiplier suffix present.
  switch (multiplier) {
  case '}':
    break;
  case 'k':
  case 'K':
    length <<= 10;
    break;
  case 'm':
  case 'M':
    length <<= 20;
    break;
  case 'g':
  case 'G':
    length <<= 30;
    break;
  case 't':
  case 'T':
    length <<= 40;
    break;
  default:
    fprintf(stderr, "Invalid length multiplier '%c'\n", multiplier);
    exit(1);
  }
  return length;
}

/**********************************************************************/
// Return length scaled by amount indicated by multiplier suffix.
// Exit on non-multiples of chunkSize, invalid multiplier character or 0
// length.
static uint64_t computeAndVerifyLength(uint64_t length, const char *multiplier,
                                       const char *name, size_t chunkSize)
{
  if (length == 0) {
    fprintf(stderr, "Stream \"%s\" has zero length\n", name);
    exit(1);
  }
  length = applyMultiplier(multiplier[0], length);

  if (((length / ((uint64_t) chunkSize)) * ((uint64_t) chunkSize))
      != length) {
    fprintf(stderr, "Stream length %" PRIu64
            " for stream \"%s\" must be a multiple of the chunk size: %zu\n",
            length, name, chunkSize);
    exit(1);
  }

  return length;
}

/**********************************************************************/
// returns true on EOF, false on successful parse, exits on error or parse
// failure
static bool parseSimpleStreamDef(AnyStreamDefHead *streamDefs,
                                 FILE             *file,
                                 const char       *name,
                                 size_t            chunkSize)
{
  AnyStreamDef    *def;
  uint64_t         length;
  char             multiplier[2];
  int              sret;
  sret = fscanf(file, " length = %" SCNu64 "%1s }", &length, multiplier);
  if (checkDone(sret, file)) {
    return true;
  }
  if (sret != 2) {
    fprintf(stderr, "Unable to parse simple stream definition for \"%s\"\n",
            name);
    exit(1);
  }
  length = computeAndVerifyLength(length, multiplier, name, chunkSize);

  // create a simple stream definition
  def = calloc(1, sizeof(AnyStreamDef));
  if (def == NULL) {
    errx(1, "Unable to allocate simple stream definition.");
  }
  def->name = strdup(name);
  if (def->name == NULL) {
    errx(1, "Unable to duplicate stream name.");
  }
  def->tag    = STREAM_TAG_SIMPLE;
  def->length = length / ((uint64_t) chunkSize);
  addStreamDef(streamDefs, def);
  return false;
}


/**********************************************************************/
// returns true on EOF, false on successful parse, exits on error or parse
// failure
static bool parseAliasStreamDef(AnyStreamDefHead *streamDefs,
                                FILE             *file,
                                const char       *name,
                                size_t            chunkSize)
{
  AnyStreamDef       *def;
  const AnyStreamDef *substreamDef;
  uint64_t            length;
  char                multiplier[2];
  char                substreamName[MAX_STREAM_NAME + 2];
  int                 sret;
  sret = fscanf(file, " substream = %s length = %" SCNu64 "%1s }",
                substreamName, &length, multiplier);
  if (checkDone(sret, file)) {
    return true;
  }
  if (sret != 3) {
    fprintf(stderr, "Unable to parse alias stream definition for \"%s\"\n",
            name);
    exit(1);
  }

  if (strlen(substreamName) > MAX_STREAM_NAME) {
    fprintf(stderr, "Substream name %s too long. Maximum length is %d\n",
            name, MAX_STREAM_NAME);
    exit(1);
  }

  length = computeAndVerifyLength(length, multiplier, name, chunkSize);

  // look up the substream name
  substreamDef = lookupStreamDef(streamDefs, substreamName);
  if (substreamDef == NULL) {
    fprintf(stderr, "No stream definition for substream %s.\n",
            substreamName);
    exit(1);
  }

  // check that the substream is not shorter than this alias
  if ((substreamDef->length * chunkSize) < length) {
    fprintf(stderr, "Alias substream %s is too short.\n",
            substreamName);
    exit(1);
  }

  // create an alias stream definition
  def = calloc(1, sizeof(AnyStreamDef));
  if (def == NULL) {
    errx(1, "Unable to allocate alias stream definition.");
  }
  def->name = strdup(name);
  if (def->name == NULL) {
    errx(1, "Unable to duplicate stream name.");
  }
  def->tag                     = STREAM_TAG_ALIAS;
  def->length                  = (length / ((uint64_t) chunkSize));
  def->stream.alias.substream  = substreamDef;
  addStreamDef(streamDefs, def);
  return false;
}

/**********************************************************************/
// returns true on EOF, false on successful parse, exits on error or parse
// failure
static bool parseRepeatingStreamDef(AnyStreamDefHead *streamDefs,
                                    FILE             *file,
                                    const char       *name)
{
  AnyStreamDef       *def;
  RepeatingStreamDef *repDef;
  const AnyStreamDef *substream;
  char                substreamName[MAX_STREAM_NAME + 2];
  int                 sret;
  char                repetitionMultiplier[2];
  uint64_t            numRepetitions;

  sret = fscanf(file, " substream = %s repetitions = %" SCNu64 "%1s }",
                substreamName, &numRepetitions, repetitionMultiplier);
  if (checkDone(sret, file)) {
    return true;
  }
  if (sret != 3) {
    fprintf(stderr, "Unable to parse repeating stream definition for \"%s\"\n",
            name);
    exit(1);
  }

  if (strlen(substreamName) > MAX_STREAM_NAME) {
    fprintf(stderr, "Substream name %s too long. Maximum length is %d\n",
            name, MAX_STREAM_NAME);
    exit(1);
  }
  numRepetitions = computeAndVerifyLength(numRepetitions, repetitionMultiplier,
                                          substreamName, 1);
  // look up the substream name
  substream = lookupStreamDef(streamDefs, substreamName);
  if (substream == NULL) {
    fprintf(stderr, "No stream definition for substream %s.\n",
            substreamName);
    exit(1);
  }

  // create a repeating stream definition
  def = calloc(1, sizeof(AnyStreamDef));
  if (def == NULL) {
    errx(1, "Unable to allocate repeating stream definition.");
  }
  def->name = strdup(name);
  if (def->name == NULL) {
    errx(1, "Unable to duplicate stream name.");
  }
  def->tag    = STREAM_TAG_REPEATING;
  def->length = substream->length * numRepetitions;

  repDef = &def->stream.repeating;
  repDef->substream       = substream;
  repDef->numRepetitions     = numRepetitions;
  addStreamDef(streamDefs, def);
  return false;
}

/**********************************************************************/
// returns true on EOF, false on successful parse, exits if read error
static bool parseShuffledStreamDef(AnyStreamDefHead *streamDefs,
                                   FILE             *file,
                                   const char       *name)
{
  AnyStreamDef       *def;
  const AnyStreamDef *substream;
  const AnyStreamDef *tempSubstreamDef;
  char                shuffling[8];
  unsigned int        chaining;
  unsigned int        seed;
  char                substreamName[MAX_STREAM_NAME + 2];
  int                 sret;
  sret = fscanf(file,
                " shuffling = %7s chaining = %u seed = %u substream = %s }",
                shuffling, &chaining, &seed, substreamName);
  if (checkDone(sret, file)) {
    return true;
  }
  if (sret != 4) {
    fprintf(stderr, "Unable to parse shuffled stream definition for \"%s\"\n",
            name);
    exit(1);
  }
  if (strlen(substreamName) > MAX_STREAM_NAME) {
    fprintf(stderr, "Substream name %s too long. Maximum length is %d\n",
            name, MAX_STREAM_NAME);
    exit(1);
  }
  if (((strcmp(shuffling, "xor") != 0)
       && (strcmp(shuffling, "reverse") != 0))) {
    fprintf(stderr, "Unrecognized shuffling type \"%s\" "
            "in shuffled stream definition for \"%s\"\n",
            shuffling, name);
    exit(1);
  }

  // look up the substream name
  substream = lookupStreamDef(streamDefs, substreamName);
  if (substream == NULL) {
    fprintf(stderr, "No stream definition for substream %s.\n",
            substreamName);
    exit(1);
  }
  // check that it's not a mixed stream, or if it's an alias stream, check that
  // it doesn't alias a mixed stream eventually
  tempSubstreamDef = substream;
  while (tempSubstreamDef->tag == STREAM_TAG_ALIAS) {
    tempSubstreamDef = tempSubstreamDef->stream.alias.substream;
  }
  if (tempSubstreamDef->tag == STREAM_TAG_MIXED) {
    fprintf(stderr, "Shuffled stream %s cannot have a mixed substream (%s).\n",
            name, substreamName);
    exit(1);
  }

  // create a shuffled stream definition
  def = calloc(1, sizeof(AnyStreamDef));
  if (def == NULL) {
    errx(1, "Unable to allocate shuffled stream definition.");
  }
  def->name = strdup(name);
  if (def->name == NULL) {
    errx(1, "Unable to duplicate stream name.");
  }
  def->tag                       = STREAM_TAG_SHUFFLED;
  def->length                    = substream->length;
  def->stream.shuffled.shuffling = ((strcmp(shuffling, "xor") == 0)
                                    ? STREAM_SHUFFLING_XOR
                                    : STREAM_SHUFFLING_REVERSE);
  def->stream.shuffled.chaining  = chaining;
  def->stream.shuffled.seed      = seed;
  def->stream.shuffled.substream = substream;
  addStreamDef(streamDefs, def);
  return false;
}

/**********************************************************************/
// returns true on EOF, false on successful parse, exits on error or parse
// failure
static bool parseMixedStreamDef(AnyStreamDefHead *streamDefs,
                                FILE             *file,
                                const char       *name)
{
  AnyStreamDef        *def;
  char                 chainType[13];
  uint64_t             mixing;
  unsigned int         seed;
  char                 substreamName[MAX_STREAM_NAME + 2];
  unsigned int         numSubstreams        = 0;
  uint64_t             length               = 0;
  int                  sret                 = 0;
  const AnyStreamDef **substreams           = NULL;
  bool                 ranOut               = false;
  unsigned int         numSubstreamsDefined = 0;

  // parse the common info
  sret = fscanf(file,
                " chaintype = %12s mixing = %" SCNu64
                " seed = %u numsubstreams = %u",
                chainType, &mixing, &seed, &numSubstreams);
  if (checkDone(sret, file)) {
    return true;
  }
  if (sret != 4) {
    fprintf(stderr, "Unable to parse mixed stream definition for \"%s\"\n",
            name);
    exit(1);
  }
  if (((strcmp(chainType, "fixed") != 0)
       && (strcmp(chainType, "fixed_length") != 0)
       && (strcmp(chainType, "random") != 0))) {
    fprintf(stderr, "Unrecognized chain type \"%s\" "
            "in mixed stream definition for \"%s\"\n",
            chainType, name);
    exit(1);
  }
  if (numSubstreams < 2) {
    fprintf(stderr,
            "Fewer than 2 substreams for mixed stream definition \"%s\"\n",
            name);
    exit(1);
  }
  substreams = calloc(numSubstreams, sizeof(AnyStreamDef *));
  if (substreams == NULL) {
    errx(1, "Unable to allocate substreams array.");
  }

  // parse substreams
  for (unsigned int i = 0; i < numSubstreams; i++) {
    if (!ranOut) {
      if (i < numSubstreams - 1) {
        sret = fscanf(file, " substream = %s", substreamName);
      } else {
        sret = fscanf(file, " substream = %s }", substreamName);
      }
      if (checkDone(sret, file)) {
        free(substreams);
        return true;
      }
      if (sret != 1) {
        if (i > 0) {
          char brace[3];
          ranOut = true;

          sret = fscanf(file, "%2s", brace);
          if ((sret != 1) || (strcmp(brace, "}") != 0)) {
            fprintf(stderr, "Missing closing brace in mixed stream definition"
                    " for \"%s\"\n", name);
            exit(1);
          }

        } else {
          fprintf(stderr, "Unable to parse substream name "
                  "in mixed stream definition for \"%s\"\n", name);
          exit(1);
        }
      }
      if (strlen(substreamName) > MAX_STREAM_NAME) {
        fprintf(stderr, "Substream name %s too long. Maximum length is %d\n",
                name, MAX_STREAM_NAME);
        exit(1);
      }
    }
    if (!ranOut) {
      substreams[i] = lookupStreamDef(streamDefs, substreamName);
      if (substreams[i] == NULL) {
        fprintf(stderr, "No stream definition for substream %s.\n",
                substreamName);
        exit(1);
      }
      ++numSubstreamsDefined;
    } else {
      substreams[i] = substreams[i % numSubstreamsDefined];
    }
    length += substreams[i]->length;
  }

  // create a mixed stream definition
  def = calloc(1, sizeof(AnyStreamDef));
  if (def == NULL) {
    errx(1, "Unable to allocate mixed stream definition.");
  }
  def->name = strdup(name);
  if (def->name == NULL) {
    errx(1, "Unable to duplicate stream name.");
  }
  def->tag                        = STREAM_TAG_MIXED;
  def->length                     = length;
  def->stream.mixed.chainType     = ((strcmp(chainType, "fixed") == 0)
                                     ? CHAIN_FIXED
                                     : (strcmp(chainType, "fixed_length") == 0
                                        ? CHAIN_FIXED_LENGTH
                                        : CHAIN_RANDOM));
  def->stream.mixed.mixing        = mixing;
  def->stream.mixed.seed          = seed;
  def->stream.mixed.numSubstreams = numSubstreams;
  def->stream.mixed.substreams    = substreams;
  addStreamDef(streamDefs, def);
  return false;
}

/**********************************************************************/
// returns true on EOF, false on successful parse, exits on error or parse
// failure
static bool parseStreamDef(AnyStreamDefHead *streamDefs,
                           FILE             *file,
                           size_t            chunkSize)
{
  int  ret, sret;
  char name[MAX_STREAM_NAME + 2];
  char type[10];
  const AnyStreamDef *prevStreamDef;

  // try parsing a stream
  sret = fscanf(file, " %s %s {", name, type);
  if (checkDone(sret, file)) {
    return true;
  }
  if (sret == 2) {
    if (strlen(name) > MAX_STREAM_NAME) {
      fprintf(stderr, "Stream name %s too long. Maximum length is %d\n",
              name, MAX_STREAM_NAME);
      exit(1);
    }
    prevStreamDef = lookupStreamDef(streamDefs, name);
    if (prevStreamDef != NULL) {
      fprintf(stderr, "Duplicate definition for stream %s not allowed.\n",
              name);
      exit(1);
    }
    if (strcmp(type, "simple") == 0) {
      ret = parseSimpleStreamDef(streamDefs, file, name, chunkSize);
    } else if (strcmp(type, "alias") == 0) {
      ret = parseAliasStreamDef(streamDefs, file, name, chunkSize);
    } else if (strcmp(type, "shuffled") == 0) {
      ret = parseShuffledStreamDef(streamDefs, file, name);
    } else if (strcmp(type, "mixed") == 0) {
      ret = parseMixedStreamDef(streamDefs, file, name);
    } else if (strcmp(type, "repeating") == 0) {
      ret = parseRepeatingStreamDef(streamDefs, file, name);
    } else {
      fprintf(stderr, "Unknown stream type %s.\n", type);
      exit(1);
    }
  } else {
    fprintf(stderr, "Unable to parse stream definition.\n");
    exit(1);
  }

  return ret;
}

/**********************************************************************/
// returns true on EOF, false on successful parse, exits on error or parse
// failure
static bool parseRun(AnyStreamDefHead *streamDefs,
                     RunQueueHead     *streamRuns,
                     FILE             *file)
{
  char                name[MAX_STREAM_NAME + 2];
  int                 sret;
  const AnyStreamDef *stream;
  AnyStreamDefQueue  *queueEntry;

  // try parsing a run
  sret = fscanf(file, " { %s", name);
  while (true) {
    if (checkDone(sret, file)) {
      return true;
    }
    if (sret != 1) {
      fprintf(stderr, "Unable to parse run definition.\n");
      exit(1);
    }
    // normal loop end condition = close brace
    if (strcmp(name, "}") == 0) {
      break;
    }
    if (strlen(name) > MAX_STREAM_NAME) {
      fprintf(stderr, "Stream name %s too long. Maximum length is %d\n",
              name, MAX_STREAM_NAME);
      exit(1);
    }
    stream = lookupStreamDef(streamDefs, name);
    if (stream == NULL) {
      fprintf(stderr, "No definition for run stream %s.\n",
              name);
      exit(1);
    }
    queueEntry = calloc(1, sizeof(AnyStreamDefQueue));
    if (queueEntry == NULL) {
      errx(1, "Unable to run queue entry.");
    }
    queueEntry->def = stream;
    STAILQ_INSERT_TAIL(streamRuns, queueEntry, link);

    // parse next
    sret = fscanf(file, " %s", name);
  };

  return false;
}

/**********************************************************************/
// returns true on EOF, false on successful parse, exits on error or parse
// failure
static bool parseComment(FILE *file)
{
  char  buf[80];
  char *ret;

  do {
    // read until we get 100 char, or hit the next newline, EOF or error
    ret = fgets(buf, 80, file);
    if (ret == NULL) {
      // error or...
      if (ferror(file) != 0) {
        errx(1, "Error while parsing config file");
      }
      // EOF
      return true;
    }
  } while (buf[strlen(buf) - 1] != '\n'); // long comment!

  return false;
}

/**********************************************************************/
// exits program on error or parse failure
static void parseTestConfig(AnyStreamDefHead *streamDefs,
			    RunQueueHead     *streamRuns,
			    FILE             *file,
			    size_t            chunkSize)
{
  while (true) {
    // get keyword
    char keyword[8];
    int sret = fscanf(file, " %7s", keyword);
    if (checkDone(sret, file)) {
      break;
    }
    if (sret == 1) {
      int ret;
      if (strcmp(keyword, "stream") == 0) {
        ret = parseStreamDef(streamDefs, file, chunkSize);
      } else if (strcmp(keyword, "run") == 0) {
        ret = parseRun(streamDefs, streamRuns, file);
      } else if (keyword[0] == '#') {
        ret = parseComment(file);
      } else {
        fprintf(stderr, "Unknown keyword %s.\n", keyword);
        exit(1);
      }
      if (ret) {
        break;
      }
    }
  }
}

/**********************************************************************/
static uint64_t genRandUint64(struct random_data *randData)
{
  int32_t             low, high, extra;
  random_r(randData, &low);
  random_r(randData, &high);
  random_r(randData, &extra);
  return (((uint64_t) extra) << 42) ^ (((uint64_t) high) << 21)
      ^ ((uint64_t) low) ^ (((uint64_t) extra) >> 22);
}

/**********************************************************************/
static void freeStream(AnyStream *stream)
{
  const AnyStreamDef *streamDef = stream->definition;

  switch (streamDef->tag) {
  case STREAM_TAG_SIMPLE:
    break;
  case STREAM_TAG_ALIAS:
    freeStream(stream->stream.alias.substream);
    break;
  case STREAM_TAG_SHUFFLED:
    freeStream(stream->stream.shuffled.substream);
    break;
  case STREAM_TAG_MIXED:
    for (unsigned int i = 0;
         i < streamDef->stream.mixed.numSubstreams;
         i++) {
      freeStream(stream->stream.mixed.substreams[i].payload);
    }
    free(stream->stream.mixed.substreams);
    break;
  case STREAM_TAG_REPEATING:
    if (stream->stream.repeating.substream) {
      freeStream(stream->stream.repeating.substream);
    }
    break;
  default:
    fprintf(stderr, "freeStream(): Invalid stream tag %d.\n",
            streamDef->tag);
    exit(1);
  }

  free(stream);
}

/**********************************************************************/
static AnyStream *instantiateStream(const AnyStreamDef *streamDef)
{
  AnyStream *stream;
  assert(streamDef != NULL);

  stream = calloc(1, sizeof(AnyStream));
  if (stream == NULL) {
    errx(1, "Unable to allocate stream instance.");
  }
  stream->definition = streamDef;

  switch (streamDef->tag) {
  case STREAM_TAG_SIMPLE:
    // nothing to do, calloc already initialized .counter to 0
    break;

  case STREAM_TAG_ALIAS:
    {
      const AliasStreamDef *aliasDef  = &streamDef->stream.alias;
      AliasStream          *aliasInst = &stream->stream.alias;

      // instantiate the substream
      aliasInst->substream = instantiateStream(aliasDef->substream);
    }
    break;

  case STREAM_TAG_SHUFFLED:
    {
      uint64_t                 temp, shift;
      struct random_data       randData;
      char                     randState[8];
      const ShuffledStreamDef *shuffledDef  = &streamDef->stream.shuffled;
      ShuffledStream          *shuffledInst = &stream->stream.shuffled;
      uint64_t                 chaining     = (uint64_t) shuffledDef->chaining;

      // because random_r() and friends are very broken
      memset(&randData, 0, sizeof(randData));
      memset(randState, 0, sizeof(randState));

      // instantiate the substream first
      shuffledInst->substream = instantiateStream(shuffledDef->substream);
      // find the highest bit of the stream length
      temp = streamDef->length;
      shift = 0;
      while (temp > 1) {
        shift++;
        temp >>= 1;
      }
      shuffledInst->highestBit = 1 << shift;
      if (shuffledDef->shuffling == STREAM_SHUFFLING_XOR) {
        // generate the xor bits
        initstate_r(shuffledDef->seed, randState, sizeof(randState), &randData);
        shuffledInst->counterXor = ((genRandUint64(&randData) << (chaining + 1))
                                    | (1 << chaining));
      }
    }
    break;

  case STREAM_TAG_MIXED:
    {
      const MixedStreamDef *mixedDef  = &streamDef->stream.mixed;
      MixedStream          *mixedInst = &stream->stream.mixed;

      // allocate substreams array
      mixedInst->substreams = calloc(mixedDef->numSubstreams, sizeof(PstNode));
      if (mixedInst->substreams == NULL) {
        errx(1, "Unable to allocate substream tree.");
      }
      mixedInst->remainingSubstreamsLength = streamDef->length;
      mixedInst->remainingSubstreamsCount  = mixedDef->numSubstreams;
      // initialize the tree nodes and instantiate the substreams
      for (unsigned int i = 0;
           i < mixedDef->numSubstreams;
           i++) {
        mixedInst->substreams[i].length = mixedDef->substreams[i]->length;
        mixedInst->substreams[i].payload =
          (void *) instantiateStream(mixedDef->substreams[i]);
      }
      // construct the tree
      mixedInst->rootSubstream = pstConstruct(mixedInst->substreams,
                                              mixedDef->numSubstreams);
      // initialize random number generator
      memset(&mixedInst->randData, 0, sizeof(mixedInst->randData));
      memset(mixedInst->randState, 0, sizeof(mixedInst->randState));
      initstate_r(mixedDef->seed, mixedInst->randState,
                  sizeof(mixedInst->randState), &mixedInst->randData);
      // selected already set to zero by calloc
      if (mixedDef->chainType == CHAIN_FIXED) {
        uint64_t mixing = mixedDef->mixing;
        mixedInst->chainRemainder = (mixing == 0)
          ? 0
          : (1 + (mixedDef->substreams[0]->length / mixing));
      }
      else if (mixedDef->chainType == CHAIN_FIXED_LENGTH) {
        mixedInst->chainRemainder = mixedDef->mixing;
      }
    }
    break;

    case STREAM_TAG_REPEATING:
    {
      const RepeatingStreamDef *repeatingDef = &streamDef->stream.repeating;
      RepeatingStream *repeatingInst         = &stream->stream.repeating;

      // instantiate the substream
      repeatingInst->substream = instantiateStream(repeatingDef->substream);
      repeatingInst->numRepetitions = repeatingDef->numRepetitions;
    }
    break;

  default:
    fprintf(stderr, "instantiateStream(): Invalid stream tag %d.\n",
            streamDef->tag);
    exit(1);
  }

  return stream;
}

/**********************************************************************/
static AnyStream *nextStream(RunQueueHead *streamRuns)
{
  AnyStreamDefQueue  *queueEntry;
  const AnyStreamDef *streamDef = NULL;
  if (!STAILQ_EMPTY(streamRuns)) {
    queueEntry = STAILQ_FIRST(streamRuns);
    STAILQ_REMOVE_HEAD(streamRuns, link);
    streamDef = queueEntry->def;
    free(queueEntry);
  }
  return ((streamDef != NULL) ? instantiateStream(streamDef) : NULL);
}

/**********************************************************************/
static void pickNextSubstream(MixedStream          *mixedInst,
                              const MixedStreamDef *mixedDef)
{
  uint64_t randomUint63 = genRandUint64(&mixedInst->randData) & ~(1ULL << 63);
  uint64_t coinOffset = randomUint63
    / ((1ULL << 63) / mixedInst->remainingSubstreamsLength);

  // Note: It's OK to allow selection of the same substream now.  This
  // is necessary to generate the correct sampling distribution across
  // substreams to create a consistent dedupe pattern across the run.
  PstNode *node = pstSearch(mixedInst->rootSubstream, coinOffset);
  mixedInst->selected = node - mixedInst->substreams;
  if (mixedDef->chainType == CHAIN_FIXED) {
    mixedInst->chainRemainder = (mixedDef->mixing == 0)
      ? 0 : (node->length / mixedDef->mixing);
  }
  else if (mixedDef->chainType == CHAIN_FIXED_LENGTH) {
    mixedInst->chainRemainder = mixedDef->mixing;
  }
}

/**********************************************************************/
static bool nextChunkInfo(AnyStream *stream, const char **name,
			  uint64_t *counter)
{
  bool                done      = false;
  const AnyStreamDef *streamDef = stream->definition;

  switch (streamDef->tag) {

  case STREAM_TAG_SIMPLE:
    {
      SimpleStream    *simpleInst = &stream->stream.simple;

      if (simpleInst->counter < streamDef->length) {
        *name = streamDef->name;
        *counter = simpleInst->counter++;
        done = false;
      } else {
        done = true;
      }
    }
    break;

  case STREAM_TAG_ALIAS:
    {
      const AliasStreamDef *aliasDef  = &streamDef->stream.alias;
      AliasStream          *aliasInst = &stream->stream.alias;

      if (aliasInst->counter < streamDef->length) {
        *name = aliasDef->substream->name;
        *counter = aliasInst->counter++;
        done = false;
      } else {
        done = true;
      }
    }
    break;

  case STREAM_TAG_SHUFFLED:
    {
      uint64_t                 unshuffledCounter;
      const ShuffledStreamDef *shuffledDef  = &streamDef->stream.shuffled;
      ShuffledStream          *shuffledInst = &stream->stream.shuffled;

      done = nextChunkInfo(shuffledInst->substream, name, &unshuffledCounter);
      if (!done) {
        /* Apply xor bits or reverse to the counter to shuffle things, but
         * this is tricky if length is not a power of two, because an arbitrary
         * xor could make the counter bigger than the length.
         *
         * What's needed is a mask to limit the bitwise operation.  The mask
         * size is determined by the highest-order bit that's different between
         * the length and the counter itself (the mask is one minus the power
         * of two represented by that bit).  This can be revealed quickly by
         * xoring the length and the counter, then finding the highest-order
         * bit of the result.  Note: if the length *is* a power of two, the
         * highest order bit is guaranteed different, and the mask will be as
         * big as it needs to be.
         */
        uint64_t bitProbe = shuffledInst->highestBit;
        uint64_t temp     = streamDef->length ^ unshuffledCounter;
        // this loop must terminate b/c length != counter, hence temp != 0
        while ((bitProbe & temp) == 0) {
          bitProbe >>= 1;
        }
        if (shuffledDef->shuffling == STREAM_SHUFFLING_XOR) {
          *counter = (unshuffledCounter
                      ^ (shuffledInst->counterXor & (bitProbe - 1)));
        } else {
          // do an in-place reverse of the bits (swap lowest bits for highest
          // bit, and so on until the middle)
          uint64_t hiProbe = bitProbe >> 1;
          uint64_t loProbe = 1 << shuffledDef->chaining;
          for (; hiProbe > loProbe;) {
            bool gotHi = (unshuffledCounter & hiProbe) != 0;
            bool gotLo = (unshuffledCounter & loProbe) != 0;
            if (gotLo) {
              unshuffledCounter |= hiProbe;
            } else {
              unshuffledCounter &= ~hiProbe;
            }
            if (gotHi) {
              unshuffledCounter |= loProbe;
            } else {
              unshuffledCounter &= ~loProbe;
            }
            hiProbe >>= 1;
            loProbe <<= 1;
          }
          *counter = unshuffledCounter;
        }
      }
    }
    break;

  case STREAM_TAG_MIXED:
    {
      const MixedStreamDef *mixedDef            = &streamDef->stream.mixed;
      MixedStream          *mixedInst           = &stream->stream.mixed;
      bool                  switchStreams       = false;
      uint64_t              mixing              = mixedDef->mixing;

      // roll the dice to see if we're switching streams
      if (mixedInst->remainingSubstreamsCount == 1) {
        switchStreams = false;
      } else if (mixedDef->chainType == CHAIN_FIXED
                 || mixedDef->chainType == CHAIN_FIXED_LENGTH) {
        switchStreams = (mixing == 0)
          ? false : (--mixedInst->chainRemainder == 0);
      } else {
        switchStreams = genRandUint64(&mixedInst->randData) <
          (mixing * (UINT64_MAX
                     / mixedDef->substreams[mixedInst->selected]->length));
      }
      if (switchStreams) {
        // we're switching, pick the next one based on the relative length of
        // the substreams
        pickNextSubstream(mixedInst, mixedDef);
      }

      // get the next chunk from the next selected stream
      while ((done = nextChunkInfo(
                mixedInst->substreams[mixedInst->selected].payload,
                name, counter))
             && (--mixedInst->remainingSubstreamsCount > 0)) {
        // if the stream is all used up, and there are others available, prune
        // it and select another one
        mixedInst->remainingSubstreamsLength -=
          mixedInst->substreams[mixedInst->selected].length;
        pstPrune(&mixedInst->substreams[mixedInst->selected]);
        pickNextSubstream(mixedInst, mixedDef);
      }
    }
    break;

    case STREAM_TAG_REPEATING:
    {
      const RepeatingStreamDef *repeatingDef  = &streamDef->stream.repeating;
      RepeatingStream          *repeatingInst = &stream->stream.repeating;

      while (true) {
        if (repeatingInst->numRepetitions == 0) {
          done = true;
          break;
        } else {
          // Ask the substream to generate a chunk name.
          done = nextChunkInfo(repeatingInst->substream, name, counter);
          if (!done) {
            // Got a chunk name from the substream. Nothing more to do.
            break;
          } else {
            // The substream is exhausted.
            // Account for this repetition and see if any remain.
            --repeatingInst->numRepetitions;
            if (repeatingInst->numRepetitions == 0) {
              done = true;
              break;
            }
            // More iterations remain.
            // Clean up the previous instantiation...
            freeStream(repeatingInst->substream);
            // create a new one...
            repeatingInst->substream =
              instantiateStream(repeatingDef->substream);
            // and get the next hash from it.
            continue;
          }
        }
      }
    }
    break;

  default:
    fprintf(stderr, "nextChunkInfo(): Invalid stream tag %d.\n",
            streamDef->tag);
    exit(1);
  }

  return done;
}

/**********************************************************************/
static void freeStreamDefs(AnyStreamDefHead *streamDefs)
{
  for (AnyStreamDef *streamDef = TAILQ_FIRST(streamDefs);
       streamDef != NULL;
       streamDef = TAILQ_FIRST(streamDefs)) {
    TAILQ_REMOVE(streamDefs, streamDef, links);
    free(streamDef->name);
    free(streamDef);
  }
}

/**********************************************************************/
static void freeStreamRuns(RunQueueHead *streamRuns)
{
  while (!STAILQ_EMPTY(streamRuns)) {
    AnyStreamDefQueue *queueEntry = STAILQ_FIRST(streamRuns);
    STAILQ_REMOVE_HEAD(streamRuns, link);
    free(queueEntry);
  }
}

/*
 * Return total unprocessed length of stream generation run in bytes.
 */
static uint64_t getTotalRunLength(const RunQueueHead *streamRuns)
{
  AnyStreamDefQueue *queueEntry;
  uint64_t totalRunLength = 0;

  STAILQ_FOREACH(queueEntry, streamRuns, link)
  {
    totalRunLength += queueEntry->def->length;
  }
  return totalRunLength;
}



// No synchronization is needed since this function is called before
// additional threads have been created.
void globalInitAlbGenStream(int maxJobs)
{
  if (threadAlbStreamInfo != NULL) {
    fprintf(stderr,
        "globalInitAlbGenStream: thread stream info already initialized\n");
    exit(1);
  }

  maxThreads = maxJobs;
  threadAlbStreamInfo = calloc(maxThreads, sizeof(AlbStreamInfo));

  for (int i = 0; i < maxThreads; i++) {
    TAILQ_INIT(&threadAlbStreamInfo[i].streamDefs);
    STAILQ_INIT(&threadAlbStreamInfo[i].streamRuns);
  }
}

// No synchronization needed since this function is only called at program
// termination via atexit().
void globalFreeAlbGenStream()
{
  if (threadAlbStreamInfo != NULL) {
    for (int i = 0; i < maxThreads; i++) {
      if (threadAlbStreamInfo[i].streamInst != NULL) {
        freeStream(threadAlbStreamInfo[i].streamInst);
        threadAlbStreamInfo[i].streamInst = NULL;
      }
      if (threadAlbStreamInfo[i].dataBuffer != NULL) {
        free(threadAlbStreamInfo[i].dataBuffer);
        threadAlbStreamInfo[i].dataBuffer = NULL;
      }

      freeStreamRuns(&threadAlbStreamInfo[i].streamRuns);
      freeStreamDefs(&threadAlbStreamInfo[i].streamDefs);
    }

    free(threadAlbStreamInfo);
    threadAlbStreamInfo = NULL;
    maxThreads = 0;
  }
}

// No synchronization needed is needed for this function since each
// thread only works with data in separate structures, and the
// globalInitAlbGenStream() and globalFreeAlbGenStream() functions are
// only called before and after thread creation/destruction (respectively).
void initAlbGenStream(int threadNumber,
                      FILE *rfp,
                      unsigned int blocksize,
                      unsigned int compressPercent)
{
  AlbStreamInfo *streamInfo;
  uint32_t compressSize;
  char *buffer;
  char *cursor;

  if (threadNumber < 0 || threadNumber >= maxThreads) {
    fprintf(stderr, "initAlbGenStream: invalid thread number: %d\n",
        threadNumber);
    exit(1);
  }

  streamInfo = &(threadAlbStreamInfo[threadNumber]);

  //XXX assume 4k blocks. fio supports variable blocks but
  //the albGen stream stuff assume a single block size for
  //the duration of the run.
  parseTestConfig(&(streamInfo->streamDefs),
                  &(streamInfo->streamRuns),
                  rfp, blocksize);

  if (streamInfo->dataBuffer) {
    free(streamInfo->dataBuffer);
    streamInfo->dataBuffer = NULL;
  }
  if (compressPercent < 100) {
    // If we're to use compressible data, then allocate a buffer
    // of size "blocksize" and fill it with random data except for the
    // part that's compressible.
    streamInfo->dataBuffer = (char *)malloc(blocksize);
    if (streamInfo->dataBuffer == NULL) {
      fprintf(stderr,
              "initAlbGenStream: failed to allocate data buffer");
      exit(1);
    }

    compressSize = (blocksize * compressPercent) / 100;
    buffer = streamInfo->dataBuffer + compressSize + sizeof(uint32_t);
    cursor = streamInfo->dataBuffer + blocksize;
    memset(streamInfo->dataBuffer, 0xFF, compressSize);

    while (cursor >= buffer) {
      cursor -= sizeof(uint32_t);
      *(uint32_t *)cursor = rand();
    }
  }

  // Calculate total run length for streams before any of the streams
  // in the run have been processed.
  threadAlbStreamInfo[threadNumber].totalRunLength = getTotalRunLength(
      &threadAlbStreamInfo[threadNumber].streamRuns);

  // Hack to change seed value for each thread.  Only works for
  // mixed and shuffled streams currently (since these are the only
  // ones with RNG seeds).
  setThreadSeed(&threadAlbStreamInfo[threadNumber].streamDefs, threadNumber);

  threadAlbStreamInfo[threadNumber].streamInst = nextStream(
      &threadAlbStreamInfo[threadNumber].streamRuns);
}

/**
 * Sets the current RNG seed value in each stream definition by a
 * adding a thread-specific modifier if it is a type of stream that
 * uses a seed value.
 *
 * NOTE:  This is a hack to work around deficiencies in the stream
 * data generation abstraction for Albireo testing.  Latter does not
 * provide accessor functions for the AnyStreamDef variants, and all
 * stream configuration is currently file-based.  This makes
 * thread-specific customization difficult for simple variations.
 *
 * If a greater degree of per-thread customization is needed, the
 * solution would be to create a configuration file for each thread,
 * and set up the deduplication test framework to create these in
 * a single directory with a consistent naming scheme to identify
 * each thread.  The FIO program would then need to parse this to
 * obtain the configuration data for all threads.
 **/
static void setThreadSeed(const AnyStreamDefHead *streamDefs, int threadNumber)
{
  AnyStreamDef *def;

  TAILQ_FOREACH_REVERSE(def, streamDefs, anyStreamDefHead, links)
  {
    if (def->tag == STREAM_TAG_MIXED) {
      def->stream.mixed.seed += SEED_MOD_FACTOR * threadNumber;
    } else if (def->tag == STREAM_TAG_SHUFFLED) {
      def->stream.shuffled.seed += SEED_MOD_FACTOR * threadNumber;
    }
  }
}

/**********************************************************************/
bool getNextAlbGenChunk(int threadNumber, char *buffer, unsigned int blocksize)
{
  const char *name;
  AlbStreamInfo *streamInfo;
  uint64_t counter = 0;
  const char *threadLabel = "thread";
  const size_t labelSize = 32;
  uint64_t thread_tag = threadNumber;

  if (threadNumber < 0 || threadNumber >= maxThreads) {
    fprintf(stderr, "getNextAlbGenChunk: invalid thread number: %d\n",
        threadNumber);
    exit(1);
  }

  streamInfo = &(threadAlbStreamInfo[threadNumber]);

  if (streamInfo->dataBuffer) {
    memcpy(buffer, streamInfo->dataBuffer, blocksize);
  } else {
    memset(buffer, 0, blocksize);
  }

  // Implementation below adapts the data stream generated by the albGenTest
  // functions by adding a thread-specific tag as a prefix.  This is
  // sufficient to meet testing needs for now, but if greater needs arise then
  // a refactoring of the albGenTest stream generation to allow for multiple
  // thread definitions should be considered.
  if (nextChunkInfo(threadAlbStreamInfo[threadNumber].streamInst,
                    &name, &counter)) {
    threadAlbStreamInfo[threadNumber].streamInst = nextStream(
        &threadAlbStreamInfo[threadNumber].streamRuns);
    if (threadAlbStreamInfo[threadNumber].streamInst != NULL) {
      return getNextAlbGenChunk(threadNumber, buffer, blocksize);
    }
    return true;
  }

  // Prefix the data stream chunk with a thread number tag to avoid
  // duplicate blocks across multiple threads.  This is simpler than
  // alternative methods like forcing each thread's data stream to have
  // unique names it substreams, or synchronizing access to a single stream
  // across threads.
  strncpy(buffer, threadLabel, labelSize);
  memcpy(&buffer[strlen(threadLabel)], &thread_tag, sizeof(thread_tag));
  strncpy(buffer + labelSize, name, labelSize);
  memcpy(&buffer[labelSize * 2], &counter, sizeof(counter));
  return false;
}

/**********************************************************************/
uint64_t getAlbGenTotalRunLength(int threadNumber)
{
  if (threadNumber < 0 || threadNumber >= maxThreads) {
    fprintf(stderr, "getTotalRunLength: invalid thread number: %d\n",
        threadNumber);
    exit(1);
  }

  return threadAlbStreamInfo[threadNumber].totalRunLength;
}

/**********************************************************************/
bool isAlbGenStreamEmpty(int threadNumber)
{
  if (threadNumber < 0 || threadNumber >= maxThreads) {
    fprintf(stderr, "isAlbGenStreamEmpty: invalid thread number: %d\n",
        threadNumber);
    exit(1);
  }

  return (threadAlbStreamInfo[threadNumber].streamInst == NULL);
}
