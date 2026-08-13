/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef STBPLAYER_LOADER_H
#define STBPLAYER_LOADER_H

#include <stddef.h>

#include "stbplayer/backend.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct stbp_loaded_backend;

STBP_EXPORT enum stbp_result stbp_backend_load(const char* directory,
                                                const char* backend_name,
                                                struct stbp_loaded_backend** loaded,
                                                char* error,
                                                size_t error_size);
STBP_EXPORT const struct stbp_backend_api_v1* stbp_backend_api(
    const struct stbp_loaded_backend* loaded);
STBP_EXPORT void stbp_backend_unload(struct stbp_loaded_backend* loaded);
STBP_EXPORT const char* stbp_result_string(enum stbp_result result);

#if defined(__cplusplus)
}
#endif

#endif

