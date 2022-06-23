/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "directoryReader.h"
#include "errors.h"
#include "fileUtils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "modloader.h"
#include "string-utils.h"

static int module_symbol(void *handle, const char *symbol, void **sym_addr)
{
  *sym_addr = dlsym(handle, symbol);
  if (*sym_addr == NULL) {
    return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
                                    "Cannot find module with symbol \"%s\"",
                                    symbol);
  }
  return UDS_SUCCESS;
}

static void reset_module_error(void)
{
  dlerror();
}

static const char *module_error(const char *no_current_error)
{
  const char *ret = dlerror();
  return ret ? ret : no_current_error;
}

static int open_module(const char *module_name, int flag, void **handle)
{
  *handle = dlopen(module_name, flag);
  if (*handle == NULL) {
    return uds_log_warning_strerror(UDS_EMODULE_LOAD,
                                    "Error opening module \"%s\": %s",
                                    module_name,
                                    module_error("open_module(): no previous dl error"));
  }
  return UDS_SUCCESS;
}

static void close_module(void *handle)
{
  int result = dlclose(handle);
  if (result != 0) {
    uds_log_error("dlclose() failed: %s",
                  module_error("close_module(): no previous dl error"));
  }
}

/**********************************************************************/
int load_module(const char *module_name,
                module_init_meta_func_t *meta_init_func,
                struct module **loaded_module)
{
  void *handle;
  int result = open_module(module_name, RTLD_LAZY, &handle);
  if (result != UDS_SUCCESS) {
    return result;
  }

  void *params = NULL;
  int ptype = 0;

  const char *errmsg = (*meta_init_func)(handle, &params, &ptype);

  if (errmsg) {
    return uds_log_warning_strerror(UDS_EMODULE_LOAD,
                                    "Error initializing module \"%s\": %s",
                                    module_name,
                                    errmsg);
  }

  struct module *module;
  result = UDS_ALLOCATE(1, struct module, "module", &module);
  if (result != UDS_SUCCESS) {
    close_module(handle);
    return result;
  }

  module->handle = handle;
  module->params = params;
  module->ptype = ptype;
  module->next = NULL;
  *loaded_module = module;
  return UDS_SUCCESS;
}

void unload_modules(struct module *modules, bool do_dlclose)
{
  struct module *next;
  for (struct module *module = modules; module != NULL; module = next) {
    next = module->next;
    if (do_dlclose && module->handle) {
      close_module(module->handle);
    }
    free(module);
  }
}

/**********************************************************************/
static const char *
standard_module_meta_init(void *handle, void **params, int *ptype)
{
  reset_module_error();
  void *sym;
  int result = module_symbol(handle, "initializeModule", &sym);

  if (result != UDS_SUCCESS) {
    return module_error("no initialization function found");
  }

  module_init_function_ptr_t init =
    (module_init_function_ptr_t)(intptr_t) sym;
  void *p = (*init)();
  if (p == NULL) {
    return "module initialization failed";
  }

  if (params) {
    *params = p;
  }
  if (ptype) {
    *ptype = 0;
  }

  return NULL;
}

/**********************************************************************/
int load_modules(const char *directory,
                 const char *pattern,
                 size_t *count,
                 struct module **modules)
{
  return load_generic_modules(directory, pattern,
                              standard_module_meta_init, count, modules);
}

struct loader_context {
  char *pattern_buffer;
  struct module *loaded;
  size_t module_count;
  module_init_meta_func_t *meta_func;
};

/**********************************************************************/
static bool module_dirent_processor(struct dirent *entry,
                                    const char *directory,
                                    void *context,
                                    int *result)
{
  struct loader_context *loader_context =
    (struct loader_context *) context;
  if (!file_name_match(loader_context->pattern_buffer,
           entry->d_name, 0)) {
    return false;
  }

  char *name;
  *result = uds_alloc_sprintf(__func__, &name, "%s/%s", directory,
                              entry->d_name);
  if (*result != UDS_SUCCESS) {
    return true;
  }

  struct module *new_module = NULL;
  *result = load_module(name, loader_context->meta_func, &new_module);
  free(name);
  if (*result != UDS_SUCCESS) {
    return true;
  }

  new_module->next = loader_context->loaded;
  loader_context->loaded = new_module;
  loader_context->module_count++;
  return false;
}

/**********************************************************************/
int load_generic_modules(const char *directory,
                         const char *pattern,
                         module_init_meta_func_t *meta_func,
                         size_t *count,
                         struct module **modules)
{
  struct loader_context context;
  int result = uds_alloc_sprintf("pattern buffer while loading modules",
                                 &context.pattern_buffer,
                                 "%s.so",
                                 pattern);
  if (result != UDS_SUCCESS) {
    return result;
  }

  context.loaded = NULL;
  context.module_count = 0;
  context.meta_func = meta_func;

  result = read_directory(directory, "module", module_dirent_processor,
        &context);
  free(context.pattern_buffer);

  if (result != UDS_SUCCESS) {
    unload_modules(context.loaded, true);
    return result;
  }

  *count = context.module_count;
  *modules = context.loaded;
  return UDS_SUCCESS;
}
