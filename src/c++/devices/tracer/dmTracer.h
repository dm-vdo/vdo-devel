/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <linux/bio.h>
#include <linux/module.h>

int __init tracerInit(void);
void __exit tracerExit(void);

/**
 * Forward declaration of tracer device.
 **/
struct tracerDevice;

/**
 * Returns the request queue to use for blktrace-based logging for the
 * specified bio.  This is not necessarily the request queue that can be
 * accessed directly from the bio as the bio may have been modified
 * post-original submission but rather the request queue to use for correct
 * association of log messages for tracer.
 *
 * @param [in]  tracerDevice  the tracer device instance
 *
 * @return  request queue for blktrace-based logging.
 **/
extern struct request_queue *getTracerRequestQueue(struct tracerDevice *td);

/**
 * Returns the name of the specified tracer instance.
 *
 * @param [in]  tracerDevice  the tracer device instance
 *
 * @return  the tracer instance name
 **/
extern const char *getTracerName(struct tracerDevice *td);

/**
 * Returns the count of sectors to log at.
 *
 * @param [in]  tracerDevice  the tracer device instance
 *
 * @return  the tracer's sector count
 **/
extern unsigned long getTracerSectorCount(struct tracerDevice *td);
