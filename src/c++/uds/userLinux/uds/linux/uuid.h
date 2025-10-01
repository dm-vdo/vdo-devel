/*
 * User mode version of linux/uuid.h.
 */

#ifndef LINUX_UUID_H
#define LINUX_UUID_H

#include <uuid/uuid.h>

static inline void uuid_gen(uuid_t *uuid)
{
	uuid_generate(*uuid);
}

#endif // LINUX_UUID_H
