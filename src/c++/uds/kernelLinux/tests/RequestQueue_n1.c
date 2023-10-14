// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "funnel-requestqueue.h"
#include "testPrototypes.h"

#include <linux/console.h>
#include <linux/delay.h>
#include <linux/sched/sysctl.h>
#include <linux/semaphore.h>

/*
 * The purpose of this test is to exercise the idle path of the request queue
 * code and ensure that it doesn't trigger the "task blocked for more than..."
 * warnings from the kernel.
 */

/**********************************************************************/

/*
 * Helper code: memFind.
 *
 * Break this out into a separate file if it becomes more generally useful.
 */

/**
 * Find the first occurrence of a byte string inside another byte string;
 * unlike C-style character strings, either or both inputs might not be
 * terminated by a zero byte, or may contain internal zero bytes, because the
 * console interface deals only with pointer and length and not (at least
 * obviously) ASCII NUL-terminated strings.
 *
 * @param dataPointer    address of the string buffer to look in
 * @param dataLength     length of the string buffer to look in
 * @param targetPointer  address of the string being looked for
 * @param targetLength   length of the string being looked for
 *
 * @return pointer to the located substring, or NULL if not found
 **/
static const void *memFind(const void *dataPointer, size_t dataLength,
			   const void *targetPointer, size_t targetLength)
{
  const unsigned char *data = dataPointer;
  const unsigned char *target = targetPointer;
  if (targetLength == 0) {
    // Empty strings are everywhere. Look, I found one, right here!
    return dataPointer;
  }
  /*
   * In this unit test, the string being sought is always NUL-terminated and
   * has no internal zero bytes. Because of the console API, it's not clear
   * that we could say the same about the string buffer we're searching in,
   * though. If it turns out to be the case, then "strstr" would do the job for
   * us, and probably more efficiently, and we could get rid of this loop.
   */
  while (dataLength >= targetLength) {
    // If the start byte we find is too far in the buffer, there's no room...
    void *startByte = memchr(data, target[0], dataLength - targetLength + 1);
    if (startByte == NULL) {
      return NULL;
    }
    if (memcmp(startByte, target, targetLength) == 0) {
      return startByte;
    }
    // Move forward just past this hit and try again, if there's room left.
    dataLength -= ((const unsigned char *)startByte - data) + 1;
    data = startByte + 1;
  }
  return NULL;
}

/**********************************************************************/
static void memFindTest(void)
{
  static const struct {
    const char  *dataString;
    size_t       dataLength;
    const char  *targetString;
    size_t       targetLength;
    int          expectedOffset;
  } tests[] = {
    { "test1", 5, "test1", 5, 0 },
    { "test1", 5, "test", 4, 0 },
    { "test1", 5, "es", 2, 1 },
    { "test1", 5, "1", 1, 4 },
    { "test1", 5, "2", 1, -1 },
    { "test1", 5, "1\0", 2, -1 },
    { "test1", 5, "es\0", 3, -1 },
    { "test1", 5, "\0", 1, -1 },

    { "test1\0", 6, "1\0", 2, 4 },
    { "test1\0", 6, "t\0", 2, -1 },
    { "test1\0", 6, "e", 1, 1 },
    { "test1\0", 6, "\0", 1, 5 },

    { "test1", 4, "test", 4, 0 },
    { "test1", 4, "es", 2, 1 },
    { "test1", 4, "1", 1, -1 },
    { "test1", 4, "t\0", 2, -1 },
    { "test1", 4, "es\0", 3, -1 },

    { "test12", 4, "1\0", 2, -1 },

    { "\0test1", 6, "test", 4, 1 },
    { "\0test1", 6, "es", 2, 2 },
    { "\0test1", 6, "1", 1, 5 },
    { "\0test1", 6, "1\0", 2, -1 },
    { "\0test1", 6, "es\0", 3, -1 },

    { "a\0test1", 7, "test", 4, 2 },
    { "a\0test1", 7, "es", 2, 3 },
    { "a\0test1", 7, "1", 1, 6 },
    { "a\0test1", 7, "1\0", 2, -1 },
    { "a\0test1", 7, "es\0", 3, -1 },

    { "\0\0test1", 7, "test", 4, 2 },
    { "\0\0test1", 7, "es", 2, 3 },
    { "\0\0test1", 7, "1", 1, 6 },
    { "\0\0test1", 7, "1\0", 2, -1 },
    { "\0\0test1", 7, "es\0", 3, -1 },

    { "a\0\0test1", 8, "test", 4, 3 },
    { "a\0\0test1", 8, "es", 2, 4 },
    { "a\0\0test1", 8, "1", 1, 7 },
    { "a\0\0test1", 8, "1\0", 2, -1 },
    { "a\0\0test1", 8, "es\0", 3, -1 },

    { "test1", 5, "es\0t", 4, -1 },
    { "t\0est1", 5, "es\0t", 4, -1 },

    { "repeat", 6, "eat", 3, 3 },
    { "silly", 5, "", 0, 0 },
  };

  int i;
  for (i = 0; i < ARRAY_SIZE(tests); i++) {
    const void *result = memFind(tests[i].dataString, tests[i].dataLength,
				 tests[i].targetString, tests[i].targetLength);
    if (tests[i].expectedOffset == -1) {
      CU_ASSERT_PTR_NULL(result);
    } else {
      CU_ASSERT_PTR_NOT_NULL(result);
      CU_ASSERT_EQUAL(tests[i].expectedOffset,
                      (const char *)result - tests[i].dataString);
    }
  }
}

/**********************************************************************/

/*
 * Helper code: console message examination; startConsoleMonitor,
 * finishConsoleMonitor, foundMessage.
 *
 * Break this out into a separate file if it becomes more generally useful.
 */

static bool foundBlockedMessage = false;

/**
 * Indicates whether the desired message (currently hardcoded within
 * writeMessage below) has been seen in the console output during monitoring.
 *
 * @return true iff the message was seen
 **/
static inline bool foundMessage(void)
{
  return foundBlockedMessage;
}

static int oldConsoleLogLevel = -1;
static int messageCount = 0;
/**
 * "write" callback function for the kernel console interface, used for the
 * request queue test, which just checks if the messages being printed include
 * the "blocked for more than" message we're testing for.
 *
 * We do assume the target message won't be split across two buffers submitted
 * separately.
 *
 * @param console  pointer to the console data structure
 * @param message  pointer to the message to be written
 * @param length   length of the message to be written
 **/
static void writeMessage(struct console *console, const char *message,
                         unsigned int length)
{
  const char soughtMessage[] = "blocked for more than ";
  unsigned long soughtMessageLength = sizeof(soughtMessage) - 1;
  messageCount++;

  if (oops_in_progress) {
    // If we're OOPSing, punt the test.
    return;
  }
  if (foundBlockedMessage) {
    // Already failed, ignore future messages.
    return;
  }
  if (memFind(message, length, soughtMessage, soughtMessageLength) != NULL) {
    foundBlockedMessage = true;
  }
}

/**********************************************************************/
static struct console message_trap_console = {
  .name = "message_trap",
  .write = writeMessage,
};

/**
 * Start monitoring all console output, looking for the desired message.
 **/
static void startConsoleMonitor(void)
{
  foundBlockedMessage = false;
  message_trap_console.flags |= CON_ENABLED;
  messageCount = 0;
  if (console_loglevel <= LOGLEVEL_ERR) {
    oldConsoleLogLevel = console_loglevel;
    console_loglevel = LOGLEVEL_WARNING;
    pr_info("adjusting console_loglevel from %d to %d for duration of test\n",
            oldConsoleLogLevel, console_loglevel);
  }
  register_console(&message_trap_console);
}

/**
 * Stop monitoring console output.
 **/
static void finishConsoleMonitor(void)
{
  unregister_console(&message_trap_console);
  if (oldConsoleLogLevel != -1) {
    console_loglevel = oldConsoleLogLevel;
  }
  pr_err("test console got %d messages", messageCount);
}

/**********************************************************************/
static void consoleMonitorTest(void)
{
  startConsoleMonitor();
  pr_err("testing to see if we catch: blocked for more than 0 seconds\n");

  /*
   * We can tear down our console monitor before the thread that handles
   * printing runs thus making it seem that message isn't getting to the
   * console.  Delay for a short time (1 second seems to be enough, but
   * use 2 for "certainty") to give the printing thread a chance to run.
   */
  ssleep(2);

  finishConsoleMonitor();
  /*
   * If these assertions fail, error level messages aren't getting to
   * the console, despite our fiddling with console_loglevel.
   */
  CU_ASSERT_TRUE(messageCount >= 1);
  CU_ASSERT_TRUE(foundMessage());
}

/**********************************************************************/

// Now, the "real" unit test for UDS.

// TODO: use this when the two-argument flavor is available in shipped Fedora
// static DEFINE_SEMAPHORE(requestCount, 1);
static struct semaphore requestCount = __SEMAPHORE_INITIALIZER(requestCount, 1);

/**********************************************************************/
static void idleTestWorker(struct uds_request *req)
{
  up(&requestCount);
}

/**********************************************************************/
static void idleTest(void)
{
  struct uds_request request = { .unbatched = true };
  struct uds_request_queue *queue;

  down(&requestCount);
  UDS_ASSERT_SUCCESS(uds_make_request_queue("idleTest", &idleTestWorker,
                                            &queue));
  CU_ASSERT_PTR_NOT_NULL(queue);

  /*
   * The "task blocked" message for an uninterruptible sleep would normally
   * kick in somewhere past 120 seconds, but it depends when the watchdog
   * fires, etc. It also requires that the process has woken from sleep at
   * least once.
   */
  startConsoleMonitor();
  /*
   * Poke the worker process.
   *
   * First, make sure it's running.
   */
  uds_request_queue_enqueue(queue, &request);
  down(&requestCount);
  /*
   * Delay to let it sleep a while.
   *
   * Then wake it up again.
   */
  ssleep(1);
  uds_request_queue_enqueue(queue, &request);
  down(&requestCount);
  /*
   * Okay, now the fun part. We sleep long enough to trigger a complaint if the
   * request queue code makes the mistake of using uninterruptible waits.
   *
   * The checks are done every 120 seconds by default, and they check for
   * threads blocked at least 120 seconds, by default. Since we could
   * theoretically have put the worker thread to sleep a few milliseconds after
   * the check, we may need to wait for the sum of both intervals. In
   * sufficiently new kernels, both parameters can be adjusted but the values
   * aren't exported to modules. There can also be a cap placed on the number
   * of threads examined per pass; the default is all of them, and...guess
   * what? The value isn't exported to modules.
   *
   * So we assume the defaults, and wait (up to) over 240 seconds. BUT, we need
   * to not trigger the warning ourselves---ssleep/msleep use uninterruptible
   * waits, too. So we invoke shorter waits than 120 seconds, so we keep waking
   * up ourselves, but which add up to over 240 seconds. If the message
   * actually appears sooner, then we can stop the test.
   */
  long delayTime = 250; // seconds
  long shortPause = 10; // seconds
  while ((delayTime > 0) && !foundMessage()) {
    ssleep(shortPause);
    delayTime -= shortPause;
  }
  finishConsoleMonitor();
  uds_request_queue_finish(queue);
  // Now... did any complaints get written to the console?
  CU_ASSERT_FALSE(foundMessage());
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "memFind(helper)",        memFindTest },
  { "consoleMonitor(helper)", consoleMonitorTest },
  { "idle",                   idleTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "RequestQueue_n1",
  .tests = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
