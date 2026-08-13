#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int32_t (*disp_init_fn)(void);
typedef int32_t (*disp_deinit_fn)(void);
typedef int32_t (*disp_get_virtual_screen_fn)(int, uint32_t*, uint32_t*);
typedef int32_t (*disp_set_virtual_screen_fn)(int, uint32_t, uint32_t);

enum
{
  HI_UNF_DISPLAY1 = 1,
};

static void* require_symbol(const char* name)
{
  void* symbol = dlsym(RTLD_DEFAULT, name);
  if (!symbol)
    fprintf(stderr, "stb-hisi-display: missing %s: %s\n", name, dlerror());
  return symbol;
}

static int parse_dimension(const char* value, uint32_t* result)
{
  char* end = NULL;
  errno = 0;
  const unsigned long parsed = strtoul(value, &end, 10);
  if (errno || !end || *end || parsed < 64 || parsed > 8192)
    return 0;
  *result = (uint32_t)parsed;
  return 1;
}

int main(int argc, char** argv)
{
  uint32_t requested_width = 0;
  uint32_t requested_height = 0;
  if (argc != 3 || !parse_dimension(argv[1], &requested_width) ||
      !parse_dimension(argv[2], &requested_height))
  {
    fprintf(stderr, "usage: %s WIDTH HEIGHT\n", argv[0]);
    return 2;
  }

  disp_init_fn init_display = (disp_init_fn)require_symbol("HI_UNF_DISP_Init");
  disp_deinit_fn deinit_display = (disp_deinit_fn)require_symbol("HI_UNF_DISP_DeInit");
  disp_get_virtual_screen_fn get_virtual_screen =
      (disp_get_virtual_screen_fn)require_symbol("HI_UNF_DISP_GetVirtualScreen");
  disp_set_virtual_screen_fn set_virtual_screen =
      (disp_set_virtual_screen_fn)require_symbol("HI_UNF_DISP_SetVirtualScreen");
  if (!init_display || !deinit_display || !get_virtual_screen || !set_virtual_screen)
    return 3;

  int32_t result = init_display();
  if (result != 0)
  {
    fprintf(stderr, "stb-hisi-display: HI_UNF_DISP_Init failed: 0x%08x\n", (uint32_t)result);
    return 4;
  }

  uint32_t current_width = 0;
  uint32_t current_height = 0;
  result = get_virtual_screen(HI_UNF_DISPLAY1, &current_width, &current_height);
  if (result != 0)
  {
    fprintf(stderr, "stb-hisi-display: HI_UNF_DISP_GetVirtualScreen failed: 0x%08x\n",
            (uint32_t)result);
    deinit_display();
    return 5;
  }

  if (current_width != requested_width || current_height != requested_height)
  {
    result = set_virtual_screen(HI_UNF_DISPLAY1, requested_width, requested_height);
    if (result != 0)
    {
      fprintf(stderr,
              "stb-hisi-display: HI_UNF_DISP_SetVirtualScreen(%ux%u) failed: 0x%08x\n",
              requested_width, requested_height, (uint32_t)result);
      deinit_display();
      return 6;
    }

    current_width = 0;
    current_height = 0;
    result = get_virtual_screen(HI_UNF_DISPLAY1, &current_width, &current_height);
    if (result != 0 || current_width != requested_width || current_height != requested_height)
    {
      fprintf(stderr,
              "stb-hisi-display: virtual screen verification failed: result=0x%08x size=%ux%u\n",
              (uint32_t)result, current_width, current_height);
      deinit_display();
      return 7;
    }
  }

  result = deinit_display();
  if (result != 0)
  {
    fprintf(stderr, "stb-hisi-display: HI_UNF_DISP_DeInit failed: 0x%08x\n", (uint32_t)result);
    return 8;
  }

  return 0;
}
