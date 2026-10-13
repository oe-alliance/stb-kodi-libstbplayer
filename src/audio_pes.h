/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef STBPLAYER_AUDIO_PES_H
#define STBPLAYER_AUDIO_PES_H

#include "stbplayer/audio.h"

#define STBP_AUDIO_PES_HEADER_MAX 64U

enum stbp_audio_pes_variant
{
  STBP_AUDIO_PES_NORMAL = 0,
  STBP_AUDIO_PES_DREAMBOX = 1,
  STBP_AUDIO_PES_DAGS = 2
};

int stbp_audio_codec_bypass(enum stbp_audio_codec codec, enum stbp_audio_pes_variant variant);
size_t stbp_audio_normalize_dts(uint8_t* output, size_t capacity,
                               const uint8_t* input, size_t size);
size_t stbp_audio_make_pes_header(uint8_t* header,
                                  size_t capacity,
                                  const struct stbp_audio_config* config,
                                  enum stbp_audio_pes_variant variant,
                                  size_t payload_size,
                                  int64_t pts_90k);

#endif
