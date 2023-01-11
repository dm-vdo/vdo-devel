/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <wait.h>

#include "albtest.h"
#include "assertions.h"
#include "directoryUtils.h"
#include "fileAssertions.h"
#include "fileUtils.h"
#include "processManager.h"
#include "testPrototypes.h"
#include "testUtils.h"
#include "time-utils.h"

const char *BOSTON = "I come from the city of Boston,\n"
                     "The home of the bean and the cod,\n"
                     "Where Cabots speak only to Lowells,\n"
                     "And Lowells speak only to God.\n";

const char *CROCODILE = "How doth the little crocodile\n"
                        " Improve his shining tail,\n"
                        "And pour the waters of the Nile\n"
                        " On every golden scale!\n"
                        "\n"
                        "How cheerfully he seems to grin\n"
                        " How neatly spreads his claws,\n"
                        "And welcomes little fishes in,\n"
                        " With gently smiling jaws\n";

volatile int sigusr2Counter;

/**********************************************************************/
static void handleSIGUSR2(int signum __attribute__((unused)))
{
  sigusr2Counter++;
}

/**
 * Open a pipe
 *
 * @param  rfd  The read file descriptor is returned here
 * @param  wfd  The write file descriptor is returned here
 **/
static void openPipe(int *rfd, int *wfd)
{
  int pfd[2];
  UDS_ASSERT_SYSTEM_CALL(pipe(pfd));
  *rfd = pfd[0];
  *wfd = pfd[1];
}

/**
 * Setup a SIGUSR2 handler
 **/
static void setupSIGUSR2(void)
{
  struct sigaction act;
  act.sa_handler = handleSIGUSR2;
  act.sa_flags = 0;
  UDS_ASSERT_SYSTEM_CALL(sigprocmask(SIG_BLOCK, NULL, &act.sa_mask));
  UDS_ASSERT_SYSTEM_CALL(sigaction(SIGUSR2, &act, NULL));
  sigusr2Counter = 0;
}

/**
 * Wait for a subprocess to exit
 *
 * @param  child  The process id of the child
 **/
static void waitForChild(pid_t child)
{
  int status = getStatus(child);
  CU_ASSERT_TRUE(WIFEXITED(status));
  CU_ASSERT_EQUAL(extractExitStatus(status), 0);
}

/**
 * Test read_buffer and write_buffer on a regular file
 **/
static void bufferTest(void)
{
  const char *path = "/tmp/FileUtils_t1";
  bool exists;
  int fd;
  UDS_ASSERT_SUCCESS(open_file(path, FU_CREATE_READ_WRITE, &fd));
  UDS_ASSERT_SUCCESS(file_exists(path, &exists));
  CU_ASSERT_TRUE(exists);
  remove_file(path);
  UDS_ASSERT_SUCCESS(file_exists(path, &exists));
  CU_ASSERT_FALSE(exists);

  UDS_ASSERT_SUCCESS(write_buffer(fd, BOSTON, strlen(BOSTON)));
  UDS_ASSERT_SYSTEM_CALL(lseek(fd, 0, SEEK_SET));
  UDS_ASSERT_SUCCESS(read_and_verify(fd, (const u8 *) BOSTON,
                                     strlen(BOSTON)));
  UDS_ASSERT_SUCCESS(sync_and_close_file(fd, path));
}

/**
 * Test read_buffer and write_buffer on a pipe
 **/
static void pipeBufferTest(void)
{
  int rfd, wfd;
  openPipe(&rfd, &wfd);

  pid_t child = forkChild();
  if (child == 0) {
    UDS_ASSERT_SYSTEM_CALL(close(rfd));
    UDS_ASSERT_SUCCESS(write_buffer(wfd, CROCODILE, strlen(CROCODILE)));
    UDS_ASSERT_SYSTEM_CALL(close(wfd));
    _exit(0);
  }

  UDS_ASSERT_SYSTEM_CALL(close(wfd));
  UDS_ASSERT_SUCCESS(read_and_verify(rfd, (const u8 *) CROCODILE,
                                     strlen(CROCODILE)));
  UDS_ASSERT_SYSTEM_CALL(close(rfd));

  waitForChild(child);
}

/**
 * Test read_buffer and write_buffer with an EINTR
 **/
static void eintrBufferTest(void)
{
  int rfd, wfd;
  openPipe(&rfd, &wfd);

  setupSIGUSR2();

  pid_t child = forkChild();
  if (child == 0) {
    UDS_ASSERT_SYSTEM_CALL(close(rfd));
    sleep_for(ms_to_ktime(500));
    UDS_ASSERT_SYSTEM_CALL(kill(getppid(), SIGUSR2));
    sleep_for(ms_to_ktime(500));
    UDS_ASSERT_SUCCESS(write_buffer(wfd, BOSTON, strlen(BOSTON)));
    UDS_ASSERT_SYSTEM_CALL(close(wfd));
    _exit(0);
  }

  UDS_ASSERT_SYSTEM_CALL(close(wfd));
  UDS_ASSERT_SUCCESS(read_and_verify(rfd, (const u8 *) BOSTON,
                                     strlen(BOSTON)));
  UDS_ASSERT_SYSTEM_CALL(close(rfd));

  waitForChild(child);
  CU_ASSERT_EQUAL(sigusr2Counter, 1);
}

/**
 * Test read_buffer and write_buffer with a partial buffer read
 **/
static void shortBufferTest(void)
{
  int rfd, wfd;
  openPipe(&rfd, &wfd);

  setupSIGUSR2();

  pid_t child = forkChild();
  if (child == 0) {
    int count = strlen(CROCODILE);
    int count1 = count / 2;
    int count2 = count - count1;;
    UDS_ASSERT_SYSTEM_CALL(close(rfd));
    UDS_ASSERT_SUCCESS(write_buffer(wfd, CROCODILE, count1));
    sleep_for(ms_to_ktime(500));
    UDS_ASSERT_SYSTEM_CALL(kill(getppid(), SIGUSR2));
    sleep_for(ms_to_ktime(500));
    UDS_ASSERT_SUCCESS(write_buffer(wfd, CROCODILE + count1, count2));
    UDS_ASSERT_SYSTEM_CALL(close(wfd));
    _exit(0);
  }

  UDS_ASSERT_SYSTEM_CALL(close(wfd));
  UDS_ASSERT_SUCCESS(read_and_verify(rfd, (const u8 *) CROCODILE,
                                     strlen(CROCODILE)));
  UDS_ASSERT_SYSTEM_CALL(close(rfd));

  waitForChild(child);
  CU_ASSERT_EQUAL(sigusr2Counter, 1);
}

/**
 * Test that verify can fail
 **/
static void verifyTest(void)
{
  int rfd, wfd;
  openPipe(&rfd, &wfd);

  pid_t child = forkChild();
  if (child == 0) {
    UDS_ASSERT_SYSTEM_CALL(close(rfd));
    UDS_ASSERT_SUCCESS(write_buffer(wfd, CROCODILE, strlen(CROCODILE)));
    UDS_ASSERT_SYSTEM_CALL(close(wfd));
    _exit(0);
  }

  UDS_ASSERT_SYSTEM_CALL(close(wfd));
  UDS_ASSERT_ERROR(UDS_CORRUPT_DATA,
                   read_and_verify(rfd, (const u8 *) BOSTON,
                   strlen(BOSTON)));
  UDS_ASSERT_SYSTEM_CALL(close(rfd));

  waitForChild(child);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "Regular Buffer",              bufferTest          },
  { "Pipe Buffer",                 pipeBufferTest      },
  { "Pipe Buffer with EINTR",      eintrBufferTest     },
  { "Pipe Buffer with short read", shortBufferTest     },
  { "Irregular Verify",            verifyTest          },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "FileUtils_t1",
  .tests = tests
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
