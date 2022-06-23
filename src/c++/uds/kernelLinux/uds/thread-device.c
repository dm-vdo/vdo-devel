// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "thread-device.h"

#include "thread-registry.h"

/*
 * A registry of all threads temporarily associated with particular
 * VDO devices.
 */
static struct thread_registry device_id_thread_registry;

void uds_register_thread_device_id(struct registered_thread *new_thread,
				   unsigned int *id_ptr)
{
	uds_register_thread(&device_id_thread_registry, new_thread, id_ptr);
}

void uds_unregister_thread_device_id(void)
{
	uds_unregister_thread(&device_id_thread_registry);
}

int uds_get_thread_device_id(void)
{
	const unsigned int *pointer =
		uds_lookup_thread(&device_id_thread_registry);

	return pointer ? *pointer : -1;
}

void uds_initialize_thread_device_registry(void)
{
	uds_initialize_thread_registry(&device_id_thread_registry);
}
