/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef MODLOADER_H
#define MODLOADER_H

#include "type-defs.h"

struct module {
	void *handle;        // Module handle as returned from dlopen
	void *params;        // Opaque structure returned from init
	int ptype;           // Parameter type selector from meta-init
	struct module *next;
};

/**
 *  Signature of the meta-function used to call the init function,
 *  of which there can be more than one type.
 **/
typedef const char *
module_init_meta_func_t(void *handle, void **param, int *ptype);

/**
 * Signature of init routine called after module has been loaded.
 **/
typedef void *(*module_init_function_ptr_t)(void);

/**
 * Load all modules in a directory whose files match a specified pattern,
 * using the standard meta-initalization method.
 *
 * @param directory     The directory in which to look for modules
 * @param pattern       The pattern to select which modules to load
 * @param count         A pointer to hold the number of modules loaded
 * @param modules       The modules that were loaded
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check load_modules(const char *directory,
			      const char *pattern,
			      size_t *count,
			      struct module **modules);

/**
 * Load all modules in a directory whose files match a specified pattern
 * using a caller-defined module initialization method.
 *
 * @param directory       The directory in which to look for modules
 * @param pattern         The pattern to select which modules to load
 * @param meta_init_func  A function called for each module to initialize
 *                        the module.
 * @param count           A pointer to hold the number of modules loaded
 * @param modules         The modules that were loaded
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check load_generic_modules(const char *directory,
				      const char *pattern,
				      module_init_meta_func_t *meta_init_func,
				      size_t *count,
				      struct module **modules);

/**
 * Load a module.
 *
 * @param module_name      The name of the module to load
 * @param meta_init_func   A function called for each module to initialize
 *                         the module.
 * @param loaded_module    A pointer to hold the loaded module
 *
 * @return UDS_SUCCESS or an error code
 **/
int load_module(const char *module_name,
		module_init_meta_func_t *meta_init_func,
		struct module **loaded_module);

/**
 * Unload a list of modules.
 *
 * @param modules        The modules to unload
 * @param do_dlclose     Unload shared objects
 **/
void unload_modules(struct module *modules, bool do_dlclose);

#endif
