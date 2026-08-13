/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stbplayer/loader.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void log_message(void* userdata, enum stbp_log_level level, const char* message)
{
  (void)userdata;
  fprintf(stderr, "backend[%d]: %s\n", (int)level, message != NULL ? message : "");
}

int main(int argc, char** argv)
{
  const char* directory = NULL;
  const char* name = NULL;
  struct stbp_loaded_backend* loaded = NULL;
  const struct stbp_backend_api_v1* api;
  struct stbp_host_callbacks host = {0};
  struct stbp_capabilities capabilities = {0};
  void* instance = NULL;
  char error[512];
  enum stbp_result result;
  int index;

  for (index = 1; index < argc; ++index)
  {
    if (strcmp(argv[index], "--backend") == 0 && index + 1 < argc)
      name = argv[++index];
    else if (strcmp(argv[index], "--backend-dir") == 0 && index + 1 < argc)
      directory = argv[++index];
    else
    {
      fprintf(stderr, "usage: %s --backend NAME [--backend-dir DIRECTORY]\n", argv[0]);
      return 2;
    }
  }

  if (name == NULL)
  {
    fprintf(stderr, "a backend name is required\n");
    return 2;
  }

  result = stbp_backend_load(directory, name, &loaded, error, sizeof(error));
  if (result != STBP_OK)
  {
    fprintf(stderr, "load failed: %s (%s)\n", stbp_result_string(result), error);
    return 1;
  }

  api = stbp_backend_api(loaded);
  host.struct_size = sizeof(host);
  host.log = log_message;
  result = api->create(&host, &instance);
  if (result == STBP_OK)
  {
    capabilities.struct_size = sizeof(capabilities);
    result = api->probe(instance, &capabilities);
  }

  if (result == STBP_OK)
  {
    printf("backend=%s\nversion=%s\ncodec_mask=0x%016" PRIx64
           "\nfeature_mask=0x%016" PRIx64 "\nmax_size=%ux%u\nmax_packet=%u\n",
           api->backend_name, api->backend_version, capabilities.codec_mask,
           capabilities.feature_mask, capabilities.max_width, capabilities.max_height,
           capabilities.max_packet_size);
  }
  else
  {
    fprintf(stderr, "probe failed: %s\n", stbp_result_string(result));
  }

  if (instance != NULL)
    api->destroy(instance);
  stbp_backend_unload(loaded);
  return result == STBP_OK ? 0 : 1;
}

