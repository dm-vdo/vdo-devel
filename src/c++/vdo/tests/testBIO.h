/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TEST_BIO_H
#define TEST_BIO_H

#include <linux/bio.h>

/**
 * Create an empty flush bio.
 *
 * @param endio  The endio function which will be called when the flush is
 *               complete; if NULL, the default will merely free the bio
 *
 * @return The newly created bio
 **/
struct bio *createFlushBIO(bio_end_io_t *endio);

#endif // TEST_BIO_H
