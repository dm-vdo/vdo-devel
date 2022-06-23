/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "traceLogger.h"

#include <linux/slab.h>

#include "common.h"

/**********************************************************************/
int destroyTraceLogger(TraceLogger **traceLoggerPtr)
{
  const TraceLoggerApi *api = (*traceLoggerPtr)->api;
  int result = 0;

  result = api->destroy(*traceLoggerPtr);
  if (result < 0) {
    return result;
  }

  // Free memory.
  kfree(*traceLoggerPtr);
  *traceLoggerPtr = NULL;

  return 0;
}

/**********************************************************************/
int logBioDetails(TraceLogger *traceLogger, struct bio  *bio)
{
  const TraceLoggerApi *api = traceLogger->api;
  int result = 0;

  result = api->logBio(traceLogger, bio);
  return result;
}

/**********************************************************************/
int makeTraceLogger(const TraceLoggerApi  *typeApi,
                    void                  *creationParameters,
                    TraceLogger          **traceLoggerPtr)
{
  int result = 0;

  // Allocate memory.
  *traceLoggerPtr = kzalloc(sizeof(TraceLogger), GFP_KERNEL);
  if ((*traceLoggerPtr) == NULL) {
    return -ENOMEM;
  }

  // Make the logger.
  (*traceLoggerPtr)->api = typeApi;
  result
    = (*traceLoggerPtr)->api->initialize(*traceLoggerPtr, creationParameters);
  if (result < 0) {
    // Free memory.
    kfree(*traceLoggerPtr);
    *traceLoggerPtr = NULL;
    return result;
  }

  return 0;
}
