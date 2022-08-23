// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "oldInterfaces.h"

#include "assertions.h"
#include "memory-alloc.h"
#include "uds-threads.h"

typedef struct oldRequest {
  OldDedupeBlockCallback callback;
  OldCookie              cookie;
  struct uds_request     request;
} OldRequest;

static struct semaphore requestSemaphore;

/**********************************************************************/
void initializeOldInterfaces(unsigned int requestLimit)
{
  UDS_ASSERT_SUCCESS(uds_initialize_semaphore(&requestSemaphore,
                                              requestLimit));
}

/**********************************************************************/
void uninitializeOldInterfaces(void)
{
  UDS_ASSERT_SUCCESS(uds_destroy_semaphore(&requestSemaphore));
}

/**********************************************************************/
static void newCallback(struct uds_request *request)
{
  OldRequest *or = container_of(request, OldRequest, request);
  if (or->callback != NULL) {
    or->callback(request->type, request->status, or->cookie,
                 &request->new_metadata,
                 request->found ? &request->old_metadata : NULL,
                 &request->chunk_name, NULL);
  }
  UDS_FREE(or);
  uds_release_semaphore(&requestSemaphore);
}

/**********************************************************************/
void oldPostBlockName(struct uds_index_session    *session,
                      OldCookie                    cookie,
                      struct uds_chunk_data       *blockAddress,
                      const struct uds_chunk_name *chunkName,
                      OldDedupeBlockCallback       callback)
{
  UDS_ASSERT_SUCCESS(oldPostBlockNameResult(session, cookie, blockAddress,
                                            chunkName, callback));
}

/**********************************************************************/
int oldPostBlockNameResult(struct uds_index_session    *session,
                           OldCookie                    cookie,
                           struct uds_chunk_data       *blockAddress,
                           const struct uds_chunk_name *chunkName,
                           OldDedupeBlockCallback       callback)
{
  uds_acquire_semaphore(&requestSemaphore);
  OldRequest *or;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, OldRequest, __func__, &or));
  or->callback = callback;
  or->cookie   = cookie;
  or->request.callback     = newCallback;
  or->request.chunk_name   = *chunkName;
  or->request.session      = session;
  or->request.new_metadata = *blockAddress;
  or->request.type         = UDS_POST;
  int result = uds_launch_request(&or->request);
  if (result != UDS_SUCCESS) {
    UDS_FREE(or);
    uds_release_semaphore(&requestSemaphore);
  }
  return result;
}

/**********************************************************************/
void oldUpdateBlockMapping(struct uds_index_session    *session,
                           OldCookie                    cookie,
                           const struct uds_chunk_name *blockName,
                           struct uds_chunk_data       *blockAddress,
                           OldDedupeBlockCallback       callback)
{
  uds_acquire_semaphore(&requestSemaphore);
  OldRequest *or;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, OldRequest, __func__, &or));
  or->callback = callback;
  or->cookie   = cookie;
  or->request.callback     = newCallback;
  or->request.chunk_name   = *blockName;
  or->request.session      = session;
  or->request.new_metadata = *blockAddress;
  or->request.type         = UDS_UPDATE;
  UDS_ASSERT_SUCCESS(uds_launch_request(&or->request));
}
