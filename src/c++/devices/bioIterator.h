/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef BIO_ITERATOR_H
#define BIO_ITERATOR_H

#include "common.h"

/**
 * A representation of a bio vector's pertinent data.
 **/
typedef struct {
  struct bio_vec *bvec;
  sector_t        sector;
} BioVector;

/**
 * An iterator over a bio's data.
 **/
typedef struct {
  struct bio       *bio;
  struct bvec_iter  iter;
  // Needed so we can store the return value of bio_iter_iovec.
  struct bio_vec    temp;
  // Allows normalization of the returned information which resides in
  // different locations depending on kernel version, but none of which are
  // the bio_vec.
  BioVector         vector;
} BioIterator;

/**
 * Create an iterator over a bio's data.
 *
 * @param bio   The bio to iterate over
 *
 * @return An iterator over a bio
 **/
static inline BioIterator createBioIterator(struct bio *bio)
{
  BioIterator iterator = {
    .bio   = bio,
    .iter  = bio->bi_iter,
  };
  return iterator;
}

/**
 * Get the next BioVector from the iterator, or NULL if there are no more.
 *
 * @param iterator      The iterator from which to get data
 *
 * @return The next BioVector from the iterator, or NULL.
 **/
static inline BioVector *getNextBiovec(BioIterator *iterator)
{
  sector_t sector;
  struct bio_vec *bvec;
  struct bio *bio = iterator->bio;
  if (iterator->iter.bi_size == 0) {
    return NULL;
  }

  iterator->temp = bio_iter_iovec(bio, iterator->iter);
  bvec = &iterator->temp;
  sector = iterator->iter.bi_sector;
  iterator->vector.bvec = bvec;
  iterator->vector.sector = sector;
  return &iterator->vector;
}

/**
 * Advance the iterator to the next biovec in the bio.
 *
 * @param [in,out] iterator     The iterator to advance
 **/
static inline void advanceBioIterator(BioIterator *iterator)
{
  bio_advance_iter(iterator->bio, &iterator->iter, iterator->temp.bv_len);
}

#endif /* BIO_ITERATOR_H */
