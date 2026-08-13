/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stbplayer/loader.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STBP_DEFAULT_BACKEND_DIR
#define STBP_DEFAULT_BACKEND_DIR "/usr/lib/stbplayer"
#endif

struct stbp_loaded_backend
{
  void* library;
  const struct stbp_backend_api_v1* api;
};

static void set_error(char* error, size_t error_size, const char* format, ...)
{
  va_list arguments;

  if (error == NULL || error_size == 0)
    return;

  va_start(arguments, format);
  (void)vsnprintf(error, error_size, format, arguments);
  va_end(arguments);
  error[error_size - 1] = '\0';
}

static int valid_backend_name(const char* name)
{
  const unsigned char* current = (const unsigned char*)name;

  if (current == NULL || *current == '\0')
    return 0;

  for (; *current != '\0'; ++current)
  {
    if (!isalnum(*current) && *current != '-' && *current != '_')
      return 0;
  }
  return 1;
}

static int api_has_required_functions(const struct stbp_backend_api_v1* api)
{
  return api->create != NULL && api->destroy != NULL && api->probe != NULL &&
         api->open != NULL && api->queue_packet != NULL && api->get_buffer_state != NULL &&
         api->get_status != NULL && api->flush != NULL && api->drain != NULL &&
         api->reset != NULL && api->set_speed != NULL && api->set_paused != NULL &&
         api->set_video_rect != NULL && api->set_visible != NULL && api->close != NULL;
}

enum stbp_result stbp_backend_load(const char* directory,
                                   const char* backend_name,
                                   struct stbp_loaded_backend** loaded,
                                   char* error,
                                   size_t error_size)
{
  char path[1024];
  const char* backend_directory = directory != NULL ? directory : STBP_DEFAULT_BACKEND_DIR;
  struct stbp_loaded_backend* result;
  stbp_backend_get_api_fn get_api;
  const char* dynamic_error;
  int length;

  if (loaded == NULL || !valid_backend_name(backend_name) || backend_directory[0] == '\0')
  {
    set_error(error, error_size, "invalid backend load arguments");
    return STBP_ERROR_INVALID_ARGUMENT;
  }
  *loaded = NULL;

  length = snprintf(path, sizeof(path), "%s/libstbplayer-backend-%s.so", backend_directory,
                    backend_name);
  if (length < 0 || (size_t)length >= sizeof(path))
  {
    set_error(error, error_size, "backend path is too long");
    return STBP_ERROR_INVALID_ARGUMENT;
  }

  result = (struct stbp_loaded_backend*)calloc(1, sizeof(*result));
  if (result == NULL)
  {
    set_error(error, error_size, "out of memory");
    return STBP_ERROR_BACKEND;
  }

  (void)dlerror();
  result->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (result->library == NULL)
  {
    dynamic_error = dlerror();
    set_error(error, error_size, "cannot load %s: %s", path,
              dynamic_error != NULL ? dynamic_error : "unknown loader error");
    free(result);
    return STBP_ERROR_NO_DEVICE;
  }

  (void)dlerror();
  *(void**)(&get_api) = dlsym(result->library, "stbp_backend_get_api");
  dynamic_error = dlerror();
  if (dynamic_error != NULL || get_api == NULL)
  {
    set_error(error, error_size, "%s does not export stbp_backend_get_api", path);
    dlclose(result->library);
    free(result);
    return STBP_ERROR_ABI_MISMATCH;
  }

  result->api = get_api(STBP_ABI_VERSION_1, (uint32_t)sizeof(struct stbp_backend_api_v1));
  if (result->api == NULL || result->api->abi_version != STBP_ABI_VERSION_1 ||
      result->api->struct_size < sizeof(struct stbp_backend_api_v1) ||
      result->api->backend_name == NULL || result->api->backend_version == NULL ||
      strcmp(result->api->backend_name, backend_name) != 0 ||
      !api_has_required_functions(result->api))
  {
    set_error(error, error_size, "%s has an incompatible or incomplete ABI", path);
    dlclose(result->library);
    free(result);
    return STBP_ERROR_ABI_MISMATCH;
  }

  if (error != NULL && error_size > 0)
    error[0] = '\0';
  *loaded = result;
  return STBP_OK;
}

const struct stbp_backend_api_v1* stbp_backend_api(const struct stbp_loaded_backend* loaded)
{
  return loaded != NULL ? loaded->api : NULL;
}

void stbp_backend_unload(struct stbp_loaded_backend* loaded)
{
  if (loaded == NULL)
    return;
  if (loaded->library != NULL)
    dlclose(loaded->library);
  free(loaded);
}
