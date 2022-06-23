/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef OLD_INTERFACES_H
#define OLD_INTERFACES_H

#include "uds.h"

// The opaque data passed between an asynchronous UDS call and its
// corresponding callback.
typedef void *OldCookie;

/**
 * Set up to test using the old-style interfaces.
 *
 * @param requestLimit  The maximum number of simultaneous requests to allow.
 **/
void initializeOldInterfaces(unsigned int requestLimit);

/**
 * Clean up after tests using the old-style interfaces.
 **/
void uninitializeOldInterfaces(void);

/**
 * Callback function invoked to inform the Application Software that an
 * old-style interface operation completed.
 *
 * The context, type, status, cookie, and callbackArgument parameters are
 * always set.  The rest of them depend on the request type.
 *
 *   duplicateAddress: is set except for #UDS_DELETE.
 *   canonicalAddress: is set if the chunk existed.
 *   blockName: is always set.
 *   blockLength: is zero
 *
 * On a duplicate block callback, retain the canonical address and adjust the
 * duplicate address to share data with the canonical address.
 *
 * All callbacks are invoked in one thread, so the callback function should
 * not block.  In addition, this function should not call exit() or make
 * additional calls into the index.
 *
 * @param type              The request type
 * @param status            The request status (either #UDS_SUCCESS or an error
 *                          code)
 * @param cookie            The opaque data from the call that invoked this
 *                          callback
 * @param duplicateAddress  The duplicate address, which can possibly be freed
 * @param canonicalAddress  The canonical address of the chunk
 * @param blockName         The name (hash) of the chunk being referenced, used
 *                          for unmapping or remapping this block
 * @param callbackArgument  The callbackArgument given when registering the
 *                          callback, which is always NULL.
 **/
typedef void (*OldDedupeBlockCallback)(enum uds_request_type  type,
                                       int                    status,
                                       OldCookie              cookie,
                                       struct uds_chunk_data *duplicateAddress,
                                       struct uds_chunk_data *canonicalAddress,
                                       struct uds_chunk_name *blockName,
                                       void                  *callbackArgument);

/**
 * Indexes a block name and asynchronously associates it with a particular
 * address.  This operation occurs asynchronously and is a clone of
 * udsPostBlockName.
 *
 * The Application Software provides the block's name. UDS then checks this
 * name against its index.  If the block is new, it is stored in the index.  If
 * the block is a duplicate of an indexed block, UDS returns the canonical
 * block address via the OldDedupeBlockCallback callback.
 *
 * @param [in] session      The index session
 * @param [in] cookie       Opaque data for the callback
 * @param [in] blockAddress The address of the block being referenced
 * @param [in] chunkName    The name of the block
 * @param [in] callback     The callback method
 **/
void oldPostBlockName(struct uds_index_session    *session,
                      OldCookie                    cookie,
                      struct uds_chunk_data       *blockAddress,
                      const struct uds_chunk_name *chunkName,
                      OldDedupeBlockCallback       callback);

/**
 * Indexes a block name and asynchronously associates it with a particular
 * address.  This operation occurs asynchronously and is a clone of
 * udsPostBlockName.
 *
 * The Application Software provides the block's name. UDS then checks this
 * name against its index.  If the block is new, it is stored in the index.  If
 * the block is a duplicate of an indexed block, UDS returns the canonical
 * block address via the OldDedupeBlockCallback callback.  Any error from UDS
 * is returned to the caller.
 *
 * @param [in] session      The index session
 * @param [in] cookie       Opaque data for the callback
 * @param [in] blockAddress The address of the block being referenced
 * @param [in] chunkName    The name of the block
 * @param [in] callback     The callback method
 *
 * @return  The success or failure of the operation
 **/
int oldPostBlockNameResult(struct uds_index_session    *session,
                           OldCookie                    cookie,
                           struct uds_chunk_data       *blockAddress,
                           const struct uds_chunk_name *chunkName,
                           OldDedupeBlockCallback       callback);

/**
 * Updates the mapping for a particular block.  This operation occurs
 * asynchronously and is a clone of udsUpdateBlockMapping.
 *
 * @param [in] session      The index session
 * @param [in] cookie       Opaque data for the callback
 * @param [in] blockName    The block mapping to update
 * @param [in] blockAddress The new canonical mapping for this blockName
 * @param [in] callback     The callback method
 **/
void oldUpdateBlockMapping(struct uds_index_session    *session,
                           OldCookie                    cookie,
                           const struct uds_chunk_name *blockName,
                           struct uds_chunk_data       *blockAddress,
                           OldDedupeBlockCallback       callback);

#endif /* OLD_INTERFACES_H */
