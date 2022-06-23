/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TRACE_LOGGER_API_H
#define TRACE_LOGGER_API_H

#include <linux/bio.h>

/**
 * Forward declaration of trace logger.
 **/
struct traceLogger;

/**
 * Deconstructs the specified TraceLogger.
 *
 * @param [in,out]  traceLoggerPtr  TraceLogger to destroy
 *
 * @return  0 on success, error code on error.
 **/
typedef int (*TraceLoggerDestroy)(struct traceLogger *traceLogger);

/**
 * Initializes a TraceLogger of the specified type in the specified memory.
 *
 * @param [in]  traceLogger         the memory in which to make the TraceLogger
 * @param [in]  creationParameters  the parameters necessary to construct a
 *                                  TraceLogger of the specified type; may be
 *                                  NULL if the TraceLogger type does not have
 *                                  creation parameters
 *
 * @return  0 on success, error code on error.
 **/
typedef int (*TraceLoggerInitialize)(struct traceLogger *traceLogger,
                                     void               *creationParameters);

/**
 * Logs the pertinent tracing information of the specified bio using the
 * specified TraceLogger.
 *
 * The bio is assumed to be in the same state (as far as io-related parameters
 * are concerned) as when initially received by the device ultimately invoking
 * this method.  This is necessary to be able to process the data referenced by
 * the bio for logging purposes.
 *
 * @param [in]  traceLogger         TraceLogger
 * @param [in]  bio                 bio to trace
 *
 * @return  0 on success, error code on error.
 **/
typedef int (*TraceLoggerLogBio)(struct traceLogger *traceLogger,
                                 struct bio         *bio);

/**
 * Collects a TraceLogger's API methods.
 **/
typedef struct traceLoggerApi {
  TraceLoggerDestroy    destroy;
  TraceLoggerInitialize initialize;
  TraceLoggerLogBio     logBio;
} TraceLoggerApi;

#endif /* TRACE_LOGGER_API_H */
