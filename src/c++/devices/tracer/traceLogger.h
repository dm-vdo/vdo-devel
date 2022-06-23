/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TRACE_LOGGER_H
#define TRACE_LOGGER_H

#include <linux/bio.h>

#include "traceLoggerApi.h"

/**
 * The APIs declared in this header are for methods to invoke on the specified
 * type to perform the requested action.  These APIs are the entry points for
 * the type hierarchy and are to be called irrespective of the type in
 * question.  In an object-oriented sense they are the base class methods of
 * type hierarchy.
 **/

/**
 * TraceLogger
 *
 * This structure represents a TraceLogger object.
 **/
typedef struct traceLogger {
  const TraceLoggerApi *api;
  void                 *context;
} TraceLogger;

/**
 * Deconstructs the specified TraceLogger and frees the memory consumed by it.
 *
 * @param [in,out]  traceLoggerPtr  TraceLogger to destroy
 *
 * @return  0 on success, error code on error.
 **/
extern int destroyTraceLogger(TraceLogger **traceLoggerPtr);

/**
 * Logs the pertinent tracing information of the specified bio using the
 * specified TraceLogger.
 *
 * The bio must be in the same state (as far as io-related parameters are
 * concerned) as when initially received by the device invoking this method.
 * This is necessary to be able to process the data referenced by the bio
 * for logging purposes.
 *
 * @param [in]  traceLogger       TraceLogger
 * @param [in]  bio               bio to trace
 *
 * @return  0 on success, error code on error.
 **/
extern int logBioDetails(TraceLogger *traceLogger, struct bio  *bio);

#endif /* TRACE_LOGGER_H */
