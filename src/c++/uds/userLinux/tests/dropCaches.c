/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * Rerun the command under sudo
 *
 * @param argc  The number of arguments (as passed to main)
 * @param argv  The arguments (as passed to main)
 **/
static void sudoSelf(int argc, char **argv)
{
  int i;
  char *sudoArgv[argc + 2];
  sudoArgv[0] = "sudo";
  for (i = 0; i <= argc; i++) sudoArgv[i + 1] = argv[i];
  execvp(sudoArgv[0], sudoArgv);
  err(4, "execvp error");
}

/**********************************************************************/
int main(int argc, char **argv)
{
  const char *dropData = "3\n";
  const char *dropPath = "/proc/sys/vm/drop_caches";
  const ssize_t dropSize = strlen(dropData);
  if (argc != 1) {
    fprintf(stderr,"Usage:  dropCaches\n");
    return 1;
  }
  sync();
  int fd = open(dropPath, O_WRONLY);
  if (fd == -1)  {
    if (errno == EACCES) {
      sudoSelf(argc, argv);
    }
    err(2, "open(\"%s\") error", dropPath);
  }
  ssize_t n = write(fd, dropData, dropSize);
  if (n == -1) {
    err(3, "write(\"%s\") error", dropPath);
  } else if (n != dropSize) {
    errx(4, "incomplete write(\"%s\")", dropPath);
  }
  if (close(fd)) {
    err(3, "close(\"%s\") error", dropPath);
  }
  return 0;
}
