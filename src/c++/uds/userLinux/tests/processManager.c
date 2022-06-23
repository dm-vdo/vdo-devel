/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "processManager.h"

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "assertions.h"
#include "memory-alloc.h"
#include "uds-threads.h"
#include "type-defs.h"

enum {
  READ         = 0,
  WRITE        = 1,
  IGNORE_CHILD = -1,
};

static pid_t           *children   = NULL;
static unsigned int     childCount = 0;
static struct mutex     childMutex = { .mutex = UDS_MUTEX_INITIALIZER };

/**********************************************************************/
pid_t forkChild(void)
{
  //  pid_t pid = (killing ? -1 : fork());
  pid_t fork_pid = fork();
  UDS_ASSERT_SYSTEM_CALL(fork_pid);
  if (fork_pid == 0) {
    // child
    free(children);
    children = NULL;
    childCount = 0;
  } else if (fork_pid > 0) {
    // parent
    uds_lock_mutex(&childMutex);
    pid_t *newChildren;
    UDS_ASSERT_SUCCESS(uds_reallocate_memory(children,
                                             childCount * sizeof(pid_t),
                                             (childCount + 1) * sizeof(pid_t),
                                             __func__, &newChildren));
    children = newChildren;
    children[childCount++] = fork_pid;
    uds_unlock_mutex(&childMutex);
  }
  return fork_pid;
}

/**********************************************************************/
FILE *openProcessPipe(const char *command, pid_t *pidPtr)
{
  int readFds[2];
  UDS_ASSERT_SYSTEM_CALL(pipe(readFds));
  int writeFds[2];
  UDS_ASSERT_SYSTEM_CALL(pipe(writeFds));

  pid_t pid = forkChild();
  CU_ASSERT_TRUE(pid >= 0);

  if (pid == 0) {
    close(readFds[READ]);
    dup2(readFds[WRITE], WRITE);
    close(writeFds[WRITE]);
    dup2(writeFds[READ], READ);
    putenv("PS=");
    putenv("PS1=");
    putenv("PS2=");
    execl("/bin/bash", "-s", "--norc", "--noprofile", NULL);
    CU_FAIL("failed to start shell");
  }

  close(readFds[WRITE]);
  FILE *fp = fdopen(readFds[READ], "r");
  CU_ASSERT_PTR_NOT_NULL(fp);

  close(writeFds[READ]);
  FILE *write = fdopen(writeFds[WRITE], "w");
  CU_ASSERT_PTR_NOT_NULL(write);
  fprintf(write, "exec %s\n", command);
  fclose(write);

  if (pidPtr != NULL) {
    *pidPtr = pid;
  }

  return fp;
}

/**********************************************************************/
int getStatus(pid_t pid)
{
  uds_lock_mutex(&childMutex);
  int status;
  waitpid(pid, &status, 0);
  for (unsigned int i = 0; i < childCount; i++) {
    if (children[i] == pid) {
      children[i] = IGNORE_CHILD;
      break;
    }
  }
  uds_unlock_mutex(&childMutex);
  return status;
}

/**********************************************************************/
void expectStatus(pid_t pid, int expectedStatus)
{
  CU_ASSERT_EQUAL(expectedStatus, WEXITSTATUS(getStatus(pid)));
}

/**********************************************************************/
void killChildren(void)
{
  uds_lock_mutex(&childMutex);
  //  killing = true;
  for (unsigned int i = 0; i < childCount; i++) {
    if (children[i] == IGNORE_CHILD) {
      continue;
    }
    kill(children[i], SIGKILL);
    int status;
    waitpid(children[i], &status, 0);
  }

  free(children);
  children = NULL;
  childCount = 0;
  uds_unlock_mutex(&childMutex);
}
