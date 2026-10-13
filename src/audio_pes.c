/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "audio_pes.h"

#include <limits.h>
#include <string.h>

static void set_pts(uint8_t* output, int64_t pts_90k)
{
  const uint64_t pts = (uint64_t)pts_90k & UINT64_C(0x1ffffffff);

  output[0] = (uint8_t)(0x21U | ((pts >> 29U) & 0x0eU));
  output[1] = (uint8_t)(pts >> 22U);
  output[2] = (uint8_t)(0x01U | ((pts >> 14U) & 0xfeU));
  output[3] = (uint8_t)(pts >> 7U);
  output[4] = (uint8_t)(0x01U | ((pts << 1U) & 0xfeU));
}

static size_t append_pcm_format(uint8_t* output,
                                size_t capacity,
                                const struct stbp_audio_config* config,
                                enum stbp_audio_pes_variant variant,
                                size_t payload_size)
{
  const uint32_t block_align = config->channels * config->bits_per_sample / 8U;
  const uint32_t byte_rate = config->sample_rate * block_align;
  size_t cursor = 0;

  const int add_bcma_marker =
      variant == STBP_AUDIO_PES_DREAMBOX || variant == STBP_AUDIO_PES_DAGS;

  if (capacity < (add_bcma_marker ? 26U : 22U) || payload_size > UINT32_MAX)
    return 0;

  if (add_bcma_marker)
  {
    output[cursor++] = 'B';
    output[cursor++] = 'C';
    output[cursor++] = 'M';
    output[cursor++] = 'A';
  }

  output[cursor++] = (uint8_t)(payload_size >> 24U);
  output[cursor++] = (uint8_t)(payload_size >> 16U);
  output[cursor++] = (uint8_t)(payload_size >> 8U);
  output[cursor++] = (uint8_t)payload_size;

  output[cursor++] = 0x01;
  output[cursor++] = 0x00;
  output[cursor++] = (uint8_t)config->channels;
  output[cursor++] = (uint8_t)(config->channels >> 8U);
  output[cursor++] = (uint8_t)config->sample_rate;
  output[cursor++] = (uint8_t)(config->sample_rate >> 8U);
  output[cursor++] = (uint8_t)(config->sample_rate >> 16U);
  output[cursor++] = (uint8_t)(config->sample_rate >> 24U);
  output[cursor++] = (uint8_t)byte_rate;
  output[cursor++] = (uint8_t)(byte_rate >> 8U);
  output[cursor++] = (uint8_t)(byte_rate >> 16U);
  output[cursor++] = (uint8_t)(byte_rate >> 24U);
  output[cursor++] = (uint8_t)block_align;
  output[cursor++] = (uint8_t)(block_align >> 8U);
  output[cursor++] = (uint8_t)config->bits_per_sample;
  output[cursor++] = (uint8_t)(config->bits_per_sample >> 8U);
  output[cursor++] = 0x00;
  output[cursor++] = 0x00;
  return cursor;
}

int stbp_audio_codec_bypass(enum stbp_audio_codec codec, enum stbp_audio_pes_variant variant)
{
  switch (codec)
  {
    case STBP_AUDIO_CODEC_AC3:
      return 0x00;
    case STBP_AUDIO_CODEC_MPEG:
      return 0x01;
    case STBP_AUDIO_CODEC_DTS:
      return 0x02;
    case STBP_AUDIO_CODEC_EAC3:
      return variant == STBP_AUDIO_PES_DREAMBOX ? 0x07 : 0x22;
    case STBP_AUDIO_CODEC_MP3:
      return 0x0a;
    case STBP_AUDIO_CODEC_AAC:
      return 0x0b;
    case STBP_AUDIO_CODEC_PCM:
      return variant == STBP_AUDIO_PES_DREAMBOX ? 0x0f : 0x30;
    case STBP_AUDIO_CODEC_UNKNOWN:
    default:
      return -1;
  }
}

size_t stbp_audio_normalize_dts(uint8_t* output, size_t capacity,
                               const uint8_t* input, size_t size)
{
  uint32_t sync;
  uint32_t bits = 0;
  unsigned int bit_count = 0;
  size_t written = 0;
  int little_endian;
  int packed;

  if (output == NULL || input == NULL || size < 4U || capacity < size)
    return 0;
  sync = (uint32_t)input[0] << 24U | (uint32_t)input[1] << 16U |
         (uint32_t)input[2] << 8U | input[3];
  if (sync == UINT32_C(0x7ffe8001))
  {
    memcpy(output, input, size);
    return size;
  }
  little_endian = sync == UINT32_C(0xfe7f0180) || sync == UINT32_C(0xff1f00e8);
  packed = sync == UINT32_C(0x1fffe800) || sync == UINT32_C(0xff1f00e8);
  if ((!little_endian && !packed) || (size & 1U) != 0)
    return 0;

  /* Dream's decoder consumes 16-bit big-endian DTS, including DTS-CD input. */
  for (size_t i = 0; i < size; i += 2U)
  {
    const uint32_t word = little_endian ? (uint32_t)input[i + 1U] << 8U | input[i]
                                        : (uint32_t)input[i] << 8U | input[i + 1U];
    if (!packed)
    {
      output[written++] = (uint8_t)(word >> 8U);
      output[written++] = (uint8_t)word;
      continue;
    }
    bits = (bits << 14U) | (word & 0x3fffU);
    bit_count += 14U;
    while (bit_count >= 8U)
    {
      bit_count -= 8U;
      output[written++] = (uint8_t)(bits >> bit_count);
    }
  }
  return written;
}

size_t stbp_audio_make_pes_header(uint8_t* header,
                                  size_t capacity,
                                  const struct stbp_audio_config* config,
                                  enum stbp_audio_pes_variant variant,
                                  size_t payload_size,
                                  int64_t pts_90k)
{
  size_t cursor = 9U;
  size_t private_size = 0;
  size_t pes_payload_size;

  if (header == NULL || config == NULL || capacity < 14U)
    return 0;

  memset(header, 0, capacity);
  header[2] = 0x01;
  header[3] = 0xc0;
  header[6] = 0x81;

  if (pts_90k != STBP_AUDIO_PTS_NONE)
  {
    header[7] = 0x80;
    header[8] = 5;
    set_pts(header + cursor, pts_90k);
    cursor += 5U;
  }

  if (config->codec == STBP_AUDIO_CODEC_PCM)
  {
    private_size = append_pcm_format(header + cursor, capacity - cursor, config,
                                     variant, payload_size);
    if (private_size == 0)
      return 0;
    cursor += private_size;
  }

  if (payload_size > SIZE_MAX - (cursor - 6U))
    return 0;
  pes_payload_size = payload_size + cursor - 6U;
  if (pes_payload_size > UINT16_MAX)
    return 0;
  header[4] = (uint8_t)(pes_payload_size >> 8U);
  header[5] = (uint8_t)pes_payload_size;
  return cursor;
}
