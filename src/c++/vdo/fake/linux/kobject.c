// SPDX-License-Identifier: GPL-2.0-only
/*
 * A moderately heavily edited version of kobject.c - library routines
 * for handling generic kernel objects
 *
 * Copyright (c) 2002-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (c) 2006-2007 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2006-2007 Novell Inc.
 * Copyright Red Hat
 *
 */

#include <linux/kobject.h>

#include <errno.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "string-utils.h"

#include "status-codes.h"

// This needs to be initialized, somewhere... in the kernel, ksysfs does so.
static struct kobject kernel_kobject;

struct kobject *kernel_kobj = &kernel_kobject;

/**********************************************************************/
void initialize_kernel_kobject(void)
{
  kernel_kobject = (struct kobject) {
	.name = "kernel",
	.parent = NULL,
	.ktype = NULL,
	.state_initialized = 1,
	.refcount = {
		.value = 0,
	},
  };
}

/**********************************************************************/
static void kobject_init_internal(struct kobject *kobj)
{
	if (!kobj)
		return;
	memset(kobj, 0, sizeof(*kobj));
	kobj->state_initialized = 1;

	// Increment the reference count.
	kobject_get(kobj);
}

/**********************************************************************/
static int kobject_add_internal(struct kobject *kobj)
{
	int error = 0;
	struct kobject *parent;

	if (!kobj)
		return -ENOENT;

	if (!kobj->name || !kobj->name[0]) {
		uds_log_warning("kobject: (%p): attempted to be registered with empty name!\n",
				(void *) kobj);
		return -EINVAL;
	}

	parent = kobject_get(kobj->parent);

	uds_log_debug("kobject: '%s' (%p): %s: parent: '%s'\n",
		      kobj->name,
		      (void *) kobj,
		      __func__,
		      parent ? parent->name : "<NULL>");

	return error;
}

/**********************************************************************/
__attribute__((format(printf, 3, 0)))
static int kobject_add_varg(struct kobject *kobj,
			    struct kobject *parent,
			    const char *fmt, va_list vargs)
{
	char *str;
	int retval = vasprintf(&str, fmt, vargs) == -1 ? -ENOMEM : 0;
	if (retval) {
		uds_log_error("kobject: can not set name properly!\n");
		return retval;
	}

	kobj->name = str;
	kobj->parent = parent;
	return kobject_add_internal(kobj);
}

/**
 * kobject_add() - The main kobject add function.
 * @kobj: the kobject to add
 * @parent: pointer to the parent of the kobject.
 * @fmt: format to name the kobject with.
 *
 * The kobject name is set and added to the kobject hierarchy in this
 * function.
 *
 * If @parent is set, then the parent of the @kobj will be set to it.
 * If @parent is NULL, then the parent of the @kobj will be set to the
 * kobject associated with the kset assigned to this kobject.  If no kset
 * is assigned to the kobject, then the kobject will be located in the
 * root of the sysfs tree.
 *
 * Return: If this function returns an error, kobject_put() must be
 *         called to properly clean up the memory associated with the
 *         object.  Under no instance should the kobject that is passed
 *         to this function be directly freed with a call to kfree(),
 *         that can leak memory.
 *
 *         If this function returns success, kobject_put() must also be called
 *         in order to properly clean up the memory associated with the object.
 *
 *         In short, once this function is called, kobject_put() MUST be called
 *         when the use of the object is finished in order to properly free
 *         everything.
 */
int kobject_add(struct kobject *kobj, struct kobject *parent,
		const char *fmt, ...)
{
	va_list args;
	int retval;

	if (!kobj)
		return -EINVAL;

	if (!kobj->state_initialized) {
		uds_log_error("kobject '%s' (%p): tried to add an uninitialized object, something is seriously wrong.\n",
			      kobj->name, (void *) kobj);
		return -EINVAL;
	}
	va_start(args, fmt);
	retval = kobject_add_varg(kobj, parent, fmt, args);
	va_end(args);
	return retval;
}

/**
 * kobject_init() - Initialize a kobject structure.
 * @kobj: pointer to the kobject to initialize
 * @ktype: pointer to the ktype for this kobject.
 *
 * This function will properly initialize a kobject such that it can then
 * be passed to the kobject_add() call.
 *
 * After this function is called, the kobject MUST be cleaned up by a call
 * to kobject_put(), not by a call to kfree directly to ensure that all of
 * the memory is cleaned up properly.
 */
void kobject_init(struct kobject *kobj, struct kobj_type *ktype)
{
	char *err_str;

	if (!kobj) {
		err_str = "invalid kobject pointer!";
		goto error;
	}
	if (!ktype) {
		err_str = "must have a ktype to be initialized properly!\n";
		goto error;
	}
	if (kobj->state_initialized) {
		/* do not error out as sometimes we can recover */
		uds_log_error("kobject (%p): tried to init an initialized object, something is seriously wrong.\n",
			      (void *) kobj);
	}

	kobject_init_internal(kobj);
	kobj->ktype = ktype;
	return;

error:
	uds_log_error("kobject (%p): %s\n", (void *) kobj, err_str);
}

/**
 * kobject_get() - Increment refcount for object.
 * @kobj: object.
 */
struct kobject *kobject_get(struct kobject *kobj)
{
	if (kobj) {
		ASSERT_LOG_ONLY(kobj->state_initialized,
				"kobject '%s' (%p) "
				"is initialized in kobject_get()",
				kobj->name,
				(void *) kobj);
		atomic_add(1, &kobj->refcount);
	}
	return kobj;
}

/*
 * kobject_cleanup - free kobject resources.
 * @kobj: object to cleanup
 */
static void kobject_cleanup(struct kobject *kobj)
{
	struct kobject *parent = kobj->parent;
	struct kobj_type *t = kobj->ktype;
	char *name = kobj->name;

	uds_log_debug("kobject: '%s' (%p): %s, parent %p\n",
		      kobj->name,
		      (void *) kobj,
		      __func__,
		      (void *) kobj->parent);

	int result = ASSERT((t && t->release),
			    "kobject: '%s' (%p): "
			    "does not have a release() function, "
			    "it is broken and must be fixed. "
			    "See Documentation/core-api/kobject.rst.",
			    kobj->name,
			    (void *) kobj);

	if (result == VDO_SUCCESS) {
		uds_log_debug("kobject: '%s' (%p): calling ktype release\n",
			      kobj->name,
			      (void *) kobj);
		t->release(kobj);
	}

	/* free name if we allocated it */
	if (name) {
		uds_log_debug("kobject: '%s': free name\n", name);
		UDS_FREE(name);
		//kfree_const(name);
	}


	kobject_put(parent);
}

/**
 * kobject_put() - Decrement refcount for object.
 * @kobj: object.
 *
 * Decrement the refcount, and if 0, call kobject_cleanup().
 */
void kobject_put(struct kobject *kobj)
{
	if (kobj) {
		if (!kobj->state_initialized)
			uds_log_warning("kobject: '%s' (%p): is not initialized, yet kobject_put() is being called.\n",
					kobj->name, (void *) kobj);
		int count = atomic_add_return(-1, &kobj->refcount);
		ASSERT_LOG_ONLY(((count + 1) != 0),
				"kobject_put() did not decrement from 0");
		if ((count == 0) && (kobj != kernel_kobj)) {
			kobject_cleanup(kobj);
		}
	}
}
