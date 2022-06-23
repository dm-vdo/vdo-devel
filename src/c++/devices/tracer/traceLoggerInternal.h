/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TRACE_LOGGER_INTERNAL_H
#define TRACE_LOGGER_INTERNAL_H

#include "traceLogger.h"

#include <linux/bio.h>

/**
 * Constructs a TraceLogger utilizing the specified API.
 *
 * @param [in]  typeApi             API of the TraceLogger type to construct
 * @param [in]  creationParameters  the parameters necessary to construct a
 *                                  TraceLogger of the specified type; may be
 *                                  NULL if the TraceLogger type does not have
 *                                  creation parameters
 * @param [out] traceLoggerPtr      the constructed traceLogger
 *
 * @return  0 on success, error code on error.
 **/
extern int makeTraceLogger(const TraceLoggerApi  *typeApi,
                           void                  *creationParameters,
                           TraceLogger          **traceLoggerPtr);

#endif /* TRACE_LOGGER_INTERNAL_H */
