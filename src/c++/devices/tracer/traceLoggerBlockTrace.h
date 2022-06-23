/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TRACE_LOGGER_BLOCK_TRACE_H
#define TRACE_LOGGER_BLOCK_TRACE_H

#include "traceLogger.h"

/**
 * TraceLoggerBlockTraceCreationParameters
 *
 * Parameter structure passed when creating a TraceLoggerBlockTrace.
 **/
typedef struct traceLoggerBlockTraceCreationParameters {
  struct tracerDevice *tracerDevice;
} TraceLoggerBlockTraceCreationParameters;

/**
 * Constructs a TraceLogger utilizing blktrace.
 *
 * @param [in]  creationParameters  the parameters necessary to construct a
 *                                  TraceLogger of the specified type; may be
 *                                  NULL if the TraceLogger type does not have
 *                                  creation parameters
 * @param [out] traceLoggerPtr      the constructed traceLogger
 *
 * @return  0 on success, error code on error.
 **/
extern int makeBlockTraceLogger(void         *creationParameters,
                                TraceLogger **traceLoggerPtr);

#endif /* TRACE_LOGGER_BLOCK_TRACE_H */
