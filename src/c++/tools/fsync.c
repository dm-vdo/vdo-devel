/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

/*
 * fsync uses the fsync system call to ensure that files have been written
 * to stable storage.  There are two usage patterns, one for writing files
 * or directory trees. and one for removing files ore directory trees.
 *
 * For writing a file, use fsync like this:
 *
 *    $ cp source destination
 *    $ fsync destination
 *
 * The fsync command will ensure that the contents of destination are
 * written to stable storage, and that the directories above destination
 * will also be so written.  This also works for entire directory trees:
 *
 *    $ mkdir -p /u1/a/directory/tree
 *    $ echo foo >/u1/a/directory/tree/foo
 *    $ cp source /u1/a/directory/tree/file
 *    $ fsync /u1/a/directory/tree
 *
 * For removing files, use fsync like this:
 *
 *    $ rm obsolete
 *    $ fsync -rm obsolete
 *
 * The fsync command will ensure that the removal of the directory entry
 * for the given file wll be written to stable storage.  This also works
 * for directory trees:
 *
 *    $ rm -r /u1/a/directory/tree
 *    $ fsync -rm /u1/a/directory/tree
 */

#include <dirent.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char usageSummary[] =
"fsync uses the fsync system call to ensure that files have been written\n"
"to stable storage.  There are two usage patterns, one for writing files\n"
"or directory trees. and one for removing files ore directory trees.\n"
"\n"
"For writing a file, use fsync like this:\n"
"\n"
"    $ cp source destination\n"
"    $ fsync destination\n"
"\n"
"The fsync command will ensure that the contents of destination are\n"
"written to stable storage, and that the directories above destination\n"
"will also be so written.  This also works for entire directory trees:\n"
"\n"
"   $ mkdir -p /u1/a/directory/tree\n"
"   $ echo foo >/u1/a/directory/tree/foo\n"
"   $ cp source /u1/a/directory/tree/file\n"
"   $ fsync /u1/a/directory/tree\n"
"\n"
"For removing files, use fsync like this:\n"
"\n"
"   $ rm obsolete\n"
"   $ fsync -rm obsolete\n"
"\n"
"The fsync command will ensure that the removal of the directory entry\n"
"for the given file wll be written to stable storage.  This also works\n"
"for directory trees:\n"
"\n"
"   $ rm -r /u1/a/directory/tree\n"
"   $ fsync -rm /u1/a/directory/tree\n";

/**********************************************************************/
static void usage(int helpFlag)
{
  fprintf(stderr, "Usage:  fsync [--rm|--help] [path ...]\n");
  if (helpFlag) {
    fprintf(stderr, "\n%s", usageSummary);
  }
  exit(10);
}

/**********************************************************************/
static void syncFile(const char *path)
{
  int fd = open(path, O_RDONLY);
  if (fd == -1) {
    err(1, "open failure on %s", path);
  }
  if (fsync(fd) != 0) {
    err(2, "fsync failure on %s", path);
  }
  if (close(fd) != 0) {
    err(3, "close failure on %s", path);
  }
}

/**********************************************************************/
static void syncTree(const char *path)
{
  syncFile(path);
  struct stat sb;
  if (stat(path, &sb) != 0) {
    err(4, "stat failure on %s", path);
  }
  if (!S_ISDIR(sb.st_mode)) {
    return;
  }
  DIR *dir = opendir(path);
  if (dir == NULL) {
    err(5, "opendir failure on %s", path);
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, "." ) == 0) {
      continue;
    }
    if (strcmp(entry->d_name, ".." ) == 0) {
      continue;
    }
    char *path2;
    int bytesWritten = asprintf(&path2, "%s/%s", path, entry->d_name);
    if (bytesWritten < 0) {
      err(bytesWritten, "path creation failure");
    }
    syncTree(path2);
    free(path2);
  }
  if (closedir(dir) != 0) {
    err(6, "closedir failure on %s", path);
  }
}

/**********************************************************************/
static void syncDirEntry(const char *path, int allDirLevels)
{
  char *pathCopy = strdup(path);
  char *dirPath = pathCopy;
  for (int doneFlag = 0; !doneFlag;) {
    char *tail;
    if ((tail = strrchr(dirPath, '/')) == NULL) {
      dirPath = ".";
      doneFlag = 1;
    } else if (tail == dirPath) {
      dirPath = "/";
      doneFlag = 1;
    } else {
      tail[0] = '\0';
      doneFlag = !allDirLevels;
    }
    syncFile(dirPath);
  }
  free(pathCopy);
}

/**********************************************************************/
int main(int argc, char **argv)
{
  int rmFlag = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      usage(1);
    } else if (strcmp(argv[i], "--rm") == 0) {
      rmFlag = 1;
    } else if (argv[i][0] == '-') {
      usage(0);
    } else {
      char *path = argv[i];
      if (!rmFlag) {
        syncTree(path);
      }
      syncDirEntry(path, !rmFlag);
    }
  }
  return 0;
}
