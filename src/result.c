/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stbplayer/loader.h"

const char* stbp_result_string(enum stbp_result result)
{
  switch (result)
  {
    case STBP_OK:
      return "ok";
    case STBP_AGAIN:
      return "try again";
    case STBP_END_OF_STREAM:
      return "end of stream";
    case STBP_ERROR_INVALID_ARGUMENT:
      return "invalid argument";
    case STBP_ERROR_UNSUPPORTED:
      return "unsupported";
    case STBP_ERROR_NO_DEVICE:
      return "device or backend not found";
    case STBP_ERROR_BUSY:
      return "device busy";
    case STBP_ERROR_IO:
      return "I/O error";
    case STBP_ERROR_BAD_STATE:
      return "invalid state";
    case STBP_ERROR_ABI_MISMATCH:
      return "ABI mismatch";
    case STBP_ERROR_BACKEND:
      return "backend error";
    default:
      return "unknown result";
  }
}

