// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A utility to fill a UDS index with synthetic records.
 *
 * Copyright 2023 Red Hat
 */

#include <err.h>
#include <getopt.h>
#include <linux/blkdev.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <unistd.h>

#include "errors.h"
#include "fileUtils.h"
#include "indexer.h"
#include "memory-alloc.h"
#include "murmurhash3.h"

#include "constants.h"
#include "encodings.h"
#include "types.h"
#include "status-codes.h"

#include "userVDO.h"
#include "vdoVolumeUtils.h"

struct query {
  LIST_ENTRY(query) query_list;
  struct uds_request request;
};

static uint64_t data;

static pthread_mutex_t list_mutex;
static pthread_cond_t  list_cond;

LIST_HEAD(query_list, query);
static struct query_list queries = LIST_HEAD_INITIALIZER(query);

#define DEFAULT_REQUEST_LIMIT 2000

static unsigned int request_limit = DEFAULT_REQUEST_LIMIT;

static unsigned int concurrent_requests = 0;
static unsigned int peak_requests = 0;

static struct block_device *uds_device = NULL;
static bool force_rebuild = false;
static unsigned int poll_interval = 65536;
/*
 * Gets a query from the lookaside list, or allocates one if possible.
 */
static struct query *get_query(void)
{
  if (pthread_mutex_lock(&list_mutex)) {
    err(2, "Unable to lock the mutex");
  }
  struct query *query = NULL;
  do {
    query = LIST_FIRST(&queries);
    if (query) {
      LIST_REMOVE(query, query_list);
      break;
    }
    if (concurrent_requests < request_limit) {
      query = malloc(sizeof(*query));
      if (query) {
        concurrent_requests++;
        if (peak_requests < concurrent_requests) {
          peak_requests = concurrent_requests;
        }
        break;
      }
    }
    // If all else fails, wait for one to be available
    if (pthread_cond_wait(&list_cond, &list_mutex)) {
      err(2, "Unable to wait for a request");
    }
  } while (1);
  if (pthread_mutex_unlock(&list_mutex)) {
    err(2, "Unable to unlock the mutex");
  }
  return query;
}

/* Puts a query on the lookaside list. */
static void put_query(struct query *query)
{
  if (pthread_mutex_lock(&list_mutex)) {
    err(2, "Unable to lock the mutex");
  }
  LIST_INSERT_HEAD(&queries, query, query_list);
  pthread_cond_signal(&list_cond);
  if (pthread_mutex_unlock(&list_mutex)) {
    err(2, "Unable to unlock the mutex");
  }
}

static void callback(struct uds_request *request)
{
  if (request->status != UDS_SUCCESS) {
    errx(2, "Unsuccessful request %d", request->status);
  }
  struct query *query = container_of(request, struct query, request);
  put_query(query);
}

static void fill(struct uds_index_session *session)
{
  int result;
  struct uds_index_stats stats;
  time_t start_time = time(0);
  do {
    struct query *query = get_query();
    if (!query) {
      err(2, "Unable to allocate request");
    }

    query->request = (struct uds_request) {
      .callback = callback,
      .session  = session,
      .type     = UDS_POST
    };

    murmurhash3_128 (&data, sizeof(data), 0x62ea60be,
                     &query->request.record_name);
    data++;

    result = uds_launch_request(&query->request);
    if (result != UDS_SUCCESS) {
      errx(1, "Unable to start request");
    }
    /*
     * Poll the stats occasionally. As soon as an entry has been
     * discarded, the index is full.
     */
    if (!(data % poll_interval)) {
      result = uds_get_index_session_stats(session, &stats);
      if (result != UDS_SUCCESS) {
        errx(1, "Unable to get index stats");
      }
      if (stats.entries_discarded > 0)
        break;
    }
  } while (true);

  result = uds_flush_index_session(session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to flush the index session");
  }

  result = uds_get_index_session_stats(session, &stats);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to get index stats");
  }

  time_t time_passed = stats.current_time - start_time;
  printf("%lu entries added in %ldh:%ldm:%lds\n", stats.posts_not_found,
         time_passed/3600, (time_passed%3600)/60, time_passed%60);
}

static void usage(char *prog)
{
  printf("Usage: %s [OPTION]... PATH\n"
         "Fill a UDS index with synthetic data.\n"
         "\n"
         "Options:\n"
         "  --help           Print this help message and exit\n"
         "  --force-rebuild  Cause the index to rebuild on next load\n",
         prog);
}

static void read_geometry(const char *name,
                          struct volume_geometry *geometry_ptr)
{
  UserVDO *vdo;
  int result = makeVDOFromFile(name, true, &vdo);
  if (result != VDO_SUCCESS) {
    errx(1, "Could not load VDO from '%s'", name);
  }

  result = loadVolumeGeometry(vdo->layer, geometry_ptr);
  freeVDOFromFile(&vdo);
  if (result != VDO_SUCCESS) {
    errx(1, "Could not read VDO geometry from '%s'", name);
  }
}

static struct block_device *create_device(const char *name)
{
  int result;
  int fd;
  struct block_device *device;

  result = open_file(name, FU_READ_WRITE, &fd);
  if (result != UDS_SUCCESS) {
    errx(1, "%s is not a block device", name);
  }

  result = vdo_allocate(1, __func__, &device);
  if (result != VDO_SUCCESS) {
    close_file(fd, NULL);
    errx(1, "Cannot allocate device structure");
  }

  device->fd = fd;
  device->size = SIZE_MAX;
  return device;
}

static void free_device(struct block_device *device)
{
  close_file(device->fd, NULL);
  vdo_free(device);
  device = NULL;
}

static const char *parse_args(int argc, char *argv[])
{
  static const char *optstring = "fh";
  static const struct option longopts[] = {
    {"force-rebuild",   no_argument, 0, 'f'},
    {"help",            no_argument, 0, 'h'},
    {0,                 0,           0,  0 }
  };
  int opt;
  while ((opt = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {
    switch(opt) {
    case 'f':
      force_rebuild = true;
      break;
    case 'h':
      usage(argv[0]);
      exit(0);
      break;
    default:
      usage(argv[0]);
      exit(2);
      break;
    }
  }

  if (optind != argc - 1) {
    fprintf(stderr, "Exactly one PATH argument is required.\n");
    usage(argv[0]);
    exit(2);
  }

  return argv[optind++];
}

int main(int argc, char *argv[])
{
  const char *name = parse_args(argc, argv);

  int result = vdo_register_status_codes();
  if (result != VDO_SUCCESS) {
    errx(1, "Could not register VDO status codes");
  }

  struct volume_geometry geometry;
  read_geometry(name, &geometry);

  uds_device = create_device(name);

  struct uds_index_session *session;
  result = uds_create_index_session(&session);
  if (result != UDS_SUCCESS) {
    free_device(uds_device);
    errx(1, "Unable to create an index session");
  }

  block_count_t start_block
    = geometry.regions[VDO_INDEX_REGION].start_block - geometry.bio_offset;
  const struct uds_parameters params = {
    .bdev        = uds_device,
    .offset      = start_block * VDO_BLOCK_SIZE,
    .memory_size = geometry.index_config.mem,
    .sparse      = geometry.index_config.sparse,
    .nonce       = geometry.nonce,
    .zone_count  = 1,
  };

  result = uds_open_index(UDS_LOAD, &params, session);
  if (result != UDS_SUCCESS) {
    free_device(uds_device);
    errx(1, "Unable to open the index");
  }

  pthread_mutex_init(&list_mutex, NULL);
  pthread_cond_init(&list_cond, NULL);

  fill(session);

  if (!force_rebuild) {
    result = uds_close_index(session);
    if (result != UDS_SUCCESS) {
      free_device(uds_device);
      errx(1, "Unable to close the index");
    }
  }

  free_device(uds_device);
  pthread_mutex_destroy(&list_mutex);
  return 0;
}
