/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "traceLoggerBlockTrace.h"

#include <linux/bio.h>
#include <linux/blktrace_api.h>
#include <linux/device-mapper.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "bioIterator.h"
#include "common.h"
#include "dmTracer.h"
#include "MurmurHash3.h"
#include "traceLoggerInternal.h"

/**
 * TraceLoggerBlockTraceContext.
 *
 * This structure represents the context a TraceLoggerBlockTrace object
 * utilizes.
 **/

typedef struct traceLoggerBlockTraceContext {
  struct tracerDevice  *tracerDevice;
} TraceLoggerBlockTraceContext;

/**
 * TraceBlockHash
 *
 * The hash value of a block.
 **/
typedef struct traceBlockHash {
  uint64_t  high;
  uint64_t  low;
} __attribute__((packed)) TraceBlockHash;

/**********************************************************************
 * Determines whether the tracelogger is a valid instance of
 * the block trace tracelogger.
 *
 * @param [in] the trace logger to validate
 *
 * @return true if valid, false otherwise
 */
static inline bool validTraceLogger(TraceLogger *traceLogger)
{
  bool valid = traceLogger->context != NULL;

  if (valid) {
    TraceLoggerBlockTraceContext *context
      = (TraceLoggerBlockTraceContext *) traceLogger->context;
    valid = context->tracerDevice != NULL;
  }

  return valid;
}

/**********************************************************************/
static int destroyBlockTrace(TraceLogger *traceLogger)
{
  if (!validTraceLogger(traceLogger)) {
    return -EINVAL;
  }

  kfree(traceLogger->context);
  traceLogger->context = NULL;
  return 0;
}

/**********************************************************************/
static int initializeBlockTrace(TraceLogger *traceLogger,
                                void        *creationParameters)
{
  TraceLoggerBlockTraceCreationParameters *parameters;
  TraceLoggerBlockTraceContext *context;

  parameters = (TraceLoggerBlockTraceCreationParameters *) creationParameters;

  if ((parameters == NULL) || (parameters->tracerDevice == NULL)) {
    return -EINVAL;
  }

  context = kzalloc(sizeof(TraceLoggerBlockTraceContext), GFP_KERNEL);
  if (context == NULL) {
    return -ENOMEM;
  }
  context->tracerDevice = parameters->tracerDevice;

  traceLogger->context = context;

  return 0;
}

/**
 * Fills in a 4 character array as a nul-terminated string (possibly shorter
 * than 4 characters) indicating the operation specified by the input bio.
 *
 * The format of the string is: [F]<D|R|W|N>[F].  That is...
 *
 *  optional F: FLUSH
 *   <D|R|W|N>: <DISCARD|READ|WRITE|OTHER>
 *  optional F: FUA
 *
 * @param [in]  bio                 bio
 * @param [out] opString            8 character array
 **/
static void getBioOpString(struct bio *bio, char opString[])
{
  int i = 0;

  if (isFlushBio(bio)) {
    opString[i++] = 'F';
  }

  if (isWriteBio(bio)) {
    opString[i++] = 'W';
  } else if (isDiscardBio(bio)) {
    opString[i++] = 'D';
  } else if (isReadBio(bio)) {
    opString[i++] = 'R';
  } else {
    opString[i++] = 'N';
  }

  if (isFUABio(bio)) {
    opString[i++] = 'F';
  }

  opString[i] = '\0';
}

/**********************************************************************
 * Return a hash value given a set of data.
 *
 * @param [in] the data to hash
 * @param [in] the size of the data
 * @param [in] the hash seed
 * @param [out] the stored hash
 *
 **/
static int getDataHash(const char      *data,
                       int              size,
                       uint32_t         seed,
                       TraceBlockHash  *hash)
{
  MurmurHash3_x64_128(data, size, seed, hash);
  return 0;
}

/**********************************************************************/
static int logBlockTraceBioDetails(TraceLogger *traceLogger,
                                   struct bio  *bio)
{
  if (!validTraceLogger(traceLogger)) {
    return -EINVAL;
  }

  TraceLoggerBlockTraceContext *context;
  struct tracerDevice          *td;
  struct request_queue         *requestQueue;

  context = (TraceLoggerBlockTraceContext *) traceLogger->context;
  td = (struct tracerDevice *)context->tracerDevice;

  requestQueue = getTracerRequestQueue(td);

  const char *name = getTracerName(td);

  unsigned long sectorCount = getTracerSectorCount(td);

  // Get the operation identifying string.
  // We need 4 characters (including nul).
  char opString[4];
  getBioOpString(bio, opString);

  // Get an iterator over the bio.
  BioIterator iterator = createBioIterator(bio);
  BioVector *vector = getNextBiovec(&iterator);

  if (isDiscardBio(bio)) {
    blk_add_trace_msg(requestQueue,
                      "%s %llu + %llu [pbit-tracer, %s]",
                      opString,
                      (uint64_t) vector->sector,
                      (unsigned long long) to_sector(vector->bvec->bv_len),
                      name);
  } else if (isFlushBio(bio)) {
    // Device mapper splits up WRITE_FLUSH into an empty
    // flush and then a write. So we don't need to
    // handle data-containing flushes unless device-mapper
    // changes. (Good up to at least 4.18)
    BUG_ON(getBioSize(bio) > 0);
    blk_add_trace_msg(requestQueue,
                      "%s 0 + 0 [pbit-tracer, %s]",
                      opString,
                      name);
  } else if (isReadBio(bio) || isWriteBio(bio)) {
    // Iterate over the bio and log a hash value. The bio
    // should consist of one vector of the correct length
    // since we set the min and max i/o size in dmTracer.
    while (vector != NULL) {
      char           *data = bvec_kmap_local(vector->bvec);
      sector_t        vectorSectors = to_sector(vector->bvec->bv_len);
      unsigned long   length = (unsigned long)(vectorSectors / sectorCount);

      for (int i = 0; i < length; i++) {
        char *currentData = data + (i * sectorCount * SECTOR_SIZE);
        uint64_t currentSector = (uint64_t)(vector->sector + (i * sectorCount));

        TraceBlockHash hash;
        int result = getDataHash(currentData,
                                 sectorCount * SECTOR_SIZE,
                                 (uint32_t)currentSector,
                                 &hash);
        if (result < 0) {
          blk_add_trace_msg(requestQueue,
                            "%s %llu + %lu [pbit-tracer, %s],"
                              " failed to get hash; error = %d",
                            opString,
                            currentSector,
                            sectorCount,
                            name,
                            result);
        } else {
          blk_add_trace_msg(requestQueue,
                            "%s %llu + %lu [pbit-tracer, %s],"
                              " hash: %016llx%016llx",
                            opString,
                            currentSector,
                            sectorCount,
                            name,
                            hash.high,
                            hash.low);
        }
      }

      kunmap_local(data);
      advanceBioIterator(&iterator);
      vector = getNextBiovec(&iterator);
    }
  } else {
    blk_add_trace_msg(requestQueue,
                      "%s 0 + 0 [pbit-tracer, %s], unknown entry",
                      opString,
                      name);
  }

  return 0;
}

/**********************************************************************/
static const TraceLoggerApi TraceLoggerBlockTraceApi = {
  .destroy    = destroyBlockTrace,
  .initialize = initializeBlockTrace,
  .logBio     = logBlockTraceBioDetails,
};

/**********************************************************************/
int makeBlockTraceLogger(void         *creationParameters,
                         TraceLogger **traceLoggerPtr)
{
  return makeTraceLogger(&TraceLoggerBlockTraceApi,
                         creationParameters,
                         traceLoggerPtr);
}
