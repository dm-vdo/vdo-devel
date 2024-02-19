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

#define BLOCK_SIZE 4096

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
static uds_memory_config_size_t mem_size = UDS_MEMORY_CONFIG_256MB;
static uint64_t nonce = 0;
static off_t offset = 0;
static bool use_sparse = false;
static bool force_rebuild = false;
static unsigned int poll_interval;
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
         "  --help              Print this help message and exit\n"
         "  --force-rebuild     Cause the index to rebuild on next load\n"
         "  --memory-size=Size  Optional index memory size, default 0.25\n"
         "  --nonce=Nonce       The index nonce (required)\n"
         "  --offset=Bytes      The byte offset to the start of the index\n"
         "  --sparse            If the index is sparse\n",
         prog);
}

static uds_memory_config_size_t parse_mem_size(char *size_string)
{
  uds_memory_config_size_t mem_size;
  
  if (strcmp("0.25", size_string) == 0) {
    mem_size = UDS_MEMORY_CONFIG_256MB;
    poll_interval = 65536;
  } else if (strcmp("0.5", size_string) == 0
             || strcmp("0.50", size_string) == 0) {
    mem_size = UDS_MEMORY_CONFIG_512MB;
    poll_interval = 131072;
  } else if (strcmp("0.75", size_string) == 0) {
    mem_size = UDS_MEMORY_CONFIG_768MB;
    poll_interval = 196608;
  } else {
    char *endptr = NULL;
    unsigned long n = strtoul(size_string, &endptr, 10);
    if (*endptr != '\0' || n == 0 || n > UDS_MEMORY_CONFIG_MAX) {
      errx(1, "Illegal memory size, valid value: 1..%u, 0.25, 0.5, 0.75",
           UDS_MEMORY_CONFIG_MAX);
    }
    mem_size = (uds_memory_config_size_t) n;
    poll_interval = 262144;
  }
  return mem_size;
}

static uint64_t parse_nonce(char *optarg)
{
  char *endptr = NULL;
  unsigned long long n = strtoull(optarg, &endptr, 10);
  if (*endptr != '\0') {
    errx(1, "Nonce must be a non-negative integer");
  }
  return (uint64_t) n;
}

static off_t parse_offset(char *optarg)
{
  char *endptr = NULL;
  unsigned long long n = strtoull(optarg, &endptr, 10);
  if (*endptr != '\0') {
    errx(1, "Offset must be a non-negative integer");
  }
  return (off_t) n;
}

/**********************************************************************/
static struct block_device *parse_device(const char *name)
{
  int result;
  int fd;
  struct block_device *device;

  result = open_file(name, FU_READ_WRITE, &fd);
  if (result != UDS_SUCCESS) {
    errx(1, "%s is not a block device", name);
  }

  result = uds_allocate(1, struct block_device, __func__, &device);
  if (result != UDS_SUCCESS) {
    close_file(fd, NULL);
    errx(1, "Cannot allocate device structure");
  }

  device->fd = fd;
  return device;
}

static void free_device(struct block_device *device)
{
  close_file(device->fd, NULL);
  uds_free(device);
  device = NULL;
}

static void parse_args(int argc, char *argv[])
{
  static const char *optstring = "hn:o:";
  static const struct option longopts[] = {
    {"force-rebuild",   no_argument,       0,    'f'},
    {"help",            no_argument,       0,    'h'},
    {"nonce",           required_argument, 0,    'n'},
    {"offset",          required_argument, 0,    'o'},
    {"memory-size",     required_argument, 0,    'm'},
    {"sparse",          no_argument,       0,    's'},
    {0,                 0,                 0,     0 }
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
    case 'm':
      mem_size = parse_mem_size(optarg);
      break;
    case 'n':
      nonce = parse_nonce(optarg);
      break;
    case 'o':
      offset = parse_offset(optarg);
      break;
    case 's':
      use_sparse = true;
      break;
    default:
      usage(argv[0]);
      exit(2);
      break;
    }
  }

  if (!nonce) {
    fprintf(stderr, "The index nonce must be specified.\n");
    usage(argv[0]);
    exit(2);
  }
  if (optind != argc - 1) {
    fprintf(stderr, "Exactly one PATH argument is required.\n");
    usage(argv[0]);
    exit(2);
  }

  uds_device = parse_device(argv[optind]);
}

int main(int argc, char *argv[])
{
  parse_args(argc, argv);

  struct uds_index_session *session;
  int result = uds_create_index_session(&session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to create an index session");
  }

  const struct uds_parameters params = {
    .bdev        = uds_device,
    .offset      = offset,
    .memory_size = mem_size,
    .sparse      = use_sparse,
    .nonce       = nonce,
    .zone_count  = 1,
  };

  result = uds_open_index(UDS_LOAD, &params, session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to open the index");
  }
  
  pthread_mutex_init(&list_mutex, NULL);
  pthread_cond_init(&list_cond, NULL);
  
  fill(session);

  if (!force_rebuild) {
    result = uds_close_index(session);
    if (result != UDS_SUCCESS) {
      errx(1, "Unable to close the index");
    }
  }

  free_device(uds_device);
  pthread_mutex_destroy(&list_mutex);
  return 0;
}
