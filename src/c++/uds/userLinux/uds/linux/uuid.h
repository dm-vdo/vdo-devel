/*
 * Unit test requirements from linux/uuid.h.
 */

#ifndef LINUX_UUID_H
#define LINUX_UUID_H

#include <uuid/uuid.h>

static void uuid_gen(uuid_t *uuid)
{
	uuid_generate(*uuid);
}

#endif // LINUX_UUID_H
