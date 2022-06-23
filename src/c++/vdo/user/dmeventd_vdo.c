/*
 * Copyright (C) 2005-2017 Red Hat, Inc. All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <libdevmapper.h>
#include <libdevmapper-event.h>

#include "dmeventd_lvm.h"

/* First warning when data is 80% full. */
#define WARNING_THRESH  (DM_PERCENT_1 * 80)
/* Run a check every 5%. */
#define CHECK_STEP      (DM_PERCENT_1 *  5)
/* Do not bother checking data is less than 50% full. */
#define CHECK_MINIMUM   (DM_PERCENT_1 * 50)

/* Used to store all the registered vdo volumes. */
struct dso_state {
  struct dm_pool *mem;
  int percent_check;
  int percent;
  uint64_t known_size;
};

struct vdo_status {
  uint64_t used_blocks;
  uint64_t total_blocks;
};

/**
 * Skip nr fields each delimited by a single space.
 *
 * @param [in]  p        the string to work on
 * @param [in]  nr       the number of fields in the string to skip
 *
 * @return pointer to the new spot in the string after the skip
 */
static const char *skip_fields(const char *p, unsigned nr)
{
  while (p && nr-- && (p = strchr(p, ' '))) {
    p++;
  }

  return p;
}

/**
 * Count number of single-space delimited fields. The number of
 * fields is the number of spaces, plus 1.
 *
 * @param [in]  p the string to work on
 *
 * @return the count of fields in the string
 */
static unsigned count_fields(const char *p)
{
  unsigned nr = 1;

  if (!p || !*p) {
    return 0;
  }

  while ((p = skip_fields(p, 1))) {
    nr++;
  }

  return nr;
}

/**
 * Parse the dmsetup status line.
 *
 * @param [in]  params the string of parameters that dmsetup status returns
 * @param [out] the vdo status structure that gets filled in
 *
 * @returns 1 if parsing failed, 0 otherwise.
 */
static int parse_vdo_status(const char *params, struct vdo_status *status)
{
  memset(status, 0, sizeof(*status));

  if (!params) {
    log_error("Failed to parse invalid vdo params.");
    return 1;
  }

  /*
    Status output should look like this:

    <device> <operating mode> <in recovery> <index state>
    <compression state> <used physical blocks> <total physical blocks>
  */

  int nr = count_fields(params);
  if (nr != 7) {
    log_error("Status output in incorrect format: %s.", params);
    return 1;
  }

  // Skip to the datablocks used field.
  const char *skip_to = skip_fields(params, 5);
  unsigned long long used_blocks, total_blocks;

  if (sscanf(skip_to, "%llu %llu",
	     &used_blocks,
	     &total_blocks) < 2) {
    log_error("Failed to parse vdo params: %s.", params);
    return 1;
  }
  status->used_blocks = used_blocks;
  status->total_blocks = total_blocks;

  return 0;
}


/**
 * Process one vdo timer event.
 *
 * @param [in]  the state structure associated with the device
 * @param [in]  the output string from the dmsetup status call
 * @param [in]  the vdo device the event fired on
 *
 * @returns 1 if processing failed, 0 otherwise.
 */
static int process_vdo_event(struct dso_state *state,
                             char             *params,
                             const char       *device)
{
  // Parse the status string
  struct vdo_status status;
  if (parse_vdo_status((const char *)params, &status)) {
    log_error("Failed to process status line for %s.", device);
    return 1;
  }

  // VDO size had changed. Clear the threshold.
  if (state->known_size != status.total_blocks) {
    state->percent_check = CHECK_MINIMUM;
    state->known_size = status.total_blocks;
  }

  int percent = dm_make_percent(status.used_blocks, status.total_blocks);
  if ((percent >= WARNING_THRESH) && (percent > state->percent_check)) {
    /* Usage has raised more than CHECK_STEP since the last
       time. Warn users. */
    float f = dm_percent_to_float(percent);
    log_warn("WARNING: VDO %s is now %.2f%% full.", device, f);
  }

  if (percent > CHECK_MINIMUM) {
    state->percent_check = (percent / CHECK_STEP) * CHECK_STEP + CHECK_STEP;
    if (state->percent_check == DM_PERCENT_100) {
      state->percent_check--; /* Can't get bigger then 100% */
    }
  } else {
    state->percent_check = CHECK_MINIMUM;
  }

  return 0;
}

/************************************************************/
void process_event(struct dm_task *dmt,
                   enum dm_event_mask event,
                   void **user)
{
  struct dso_state *state = *user;
  void *next = NULL;
  uint64_t start, length;
  char *target_type = NULL;
  char *params;
  const char *device = dm_task_get_name(dmt);
  struct dm_task *new_dmt = NULL;

  if (event & DM_EVENT_DEVICE_ERROR) {
    // This will be hit when a dm_table_event is sent
    // from the kernel. In this case, the dm_task is
    // not the status task, so lets get that so we
    // can parse it below.
    if (!(new_dmt = dm_task_create(DM_DEVICE_STATUS))) {
      log_warn("WARNING: Can't create new task");
      return;
    }

    if (!dm_task_set_uuid(new_dmt, dm_task_get_uuid(dmt))) {
      log_warn("WARNING: Can't set name for new task");
      dm_task_destroy(new_dmt);
      return;
    }

    /* Non-blocking status read */
    if (!dm_task_no_flush(new_dmt)) {
      log_warn("WARNING: Can't set no_flush for dm status.");
      dm_task_destroy(new_dmt);
      return;
    }

    if (!dm_task_run(new_dmt)) {
      log_warn("WARNING: Can't run new task");
      dm_task_destroy(new_dmt);
      return;
    }

    dmt = new_dmt;
  }

  // Get the status output from the device
  dm_get_next_target(dmt, next, &start, &length, &target_type, &params);

  if (!target_type || (strcmp(target_type, "vdo") != 0)) {
    log_error("%s has invalid target type", device);
  } else if (process_vdo_event(state, params, device)) {
    log_error("%s event processing failed.", device);
  }

  if (new_dmt != NULL) {
    dm_task_destroy(new_dmt);
  }
}

/************************************************************
 * Important note: 1 is success, 0 is failure
 */
int register_device(const char *device,
        const char *uuid __attribute__((unused)),
        int major __attribute__((unused)),
        int minor __attribute__((unused)),
        void **user)
{

  struct dso_state *state = NULL;
  struct dm_pool *mem = NULL;

  // Set up the dso state
  if ((mem = dm_pool_create("vdo_state", 2048))
      && (state = dm_pool_zalloc(mem, sizeof(*state)))) {
    state->mem = mem;
  } else {
    if (mem != NULL) {
      dm_pool_destroy(mem);
    }
    return 0;
  }

  state->percent_check = CHECK_MINIMUM;
  *user = state;
  log_info("Monitoring vdo %s.", device);
  return 1;
}

/************************************************************
 * Important note: 1 is success, 0 is failure
 */
int unregister_device(const char *device,
                      const char *uuid __attribute__((unused)),
                      int major __attribute__((unused)),
                      int minor __attribute__((unused)),
                      void **user)
{
  struct dso_state *state = *user;

  dm_pool_destroy(state->mem);
  log_info("No longer monitoring vdo %s.", device);

  return 1;
}
