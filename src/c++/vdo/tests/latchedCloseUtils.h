/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef LATCHED_CLOSE_UTILS_H
#define LATCHED_CLOSE_UTILS_H

#include "completion.h"
#include "types.h"

/**
 * A function to launch the close of an object.
 *
 * @param context  The object to be closed
 * @param parent   The completion to notify when the close is complete
 **/
typedef void CloseLauncher(void *context, struct vdo_completion *parent);

/**
 * A function to get the closed status of an object. Returns true if the
 * object is closed.
 **/
typedef bool CloseChecker(void *context);

/**
 * A function to release a blocked IO.
 **/
typedef void BlockedIOReleaser(void *context);

typedef struct {
  /** A function to launch the close */
  CloseLauncher      *launcher;
  /** A function to check the object's closure */
  CloseChecker       *checker;
  /** Context for the launcher and checker */
  void               *closeContext;
  /** A function to release the IO being waited on */
  BlockedIOReleaser  *releaser;
  /** Context for the releaser */
  void               *releaseContext;
  /** The thread on which object closing and inspection must occur */
  thread_id_t         threadID;
} CloseInfo;

/**
 * Launch a close, verify that the object is not done closing
 * when the synchronous part of close is done, then cause the
 * condition blocking close to cease blocking and verify that
 * when the close completes that the object is closed.
 *
 * @param info    The set of functions and context needed for testing closing
 * @param result  The expected result
 **/
void runLatchedClose(CloseInfo info, int result);

#endif // LATCHED_CLOSE_UTILS_H
