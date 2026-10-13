/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef STBPLAYER_AUDIO_H
#define STBPLAYER_AUDIO_H

#include "stbplayer/backend.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define STBP_AUDIO_ABI_VERSION_1 UINT32_C(0x00010000)
#define STBP_AUDIO_PTS_NONE STBP_PTS_NONE

enum stbp_audio_codec
{
  STBP_AUDIO_CODEC_UNKNOWN = 0,
  STBP_AUDIO_CODEC_PCM = 1,
  STBP_AUDIO_CODEC_MPEG = 2,
  STBP_AUDIO_CODEC_MP3 = 3,
  STBP_AUDIO_CODEC_AAC = 4,
  STBP_AUDIO_CODEC_AC3 = 5,
  STBP_AUDIO_CODEC_EAC3 = 6,
  STBP_AUDIO_CODEC_DTS = 7
};

#define STBP_AUDIO_CODEC_BIT(codec) (UINT64_C(1) << (codec))

enum stbp_audio_sample_format
{
  STBP_AUDIO_SAMPLE_UNKNOWN = 0,
  STBP_AUDIO_SAMPLE_S16_NATIVE = 1
};

enum stbp_audio_feature
{
  STBP_AUDIO_FEATURE_HW_AV_SYNC = UINT64_C(1) << 0,
  STBP_AUDIO_FEATURE_PRESENTATION_CLOCK = UINT64_C(1) << 1,
  STBP_AUDIO_FEATURE_PAUSE = UINT64_C(1) << 2,
  STBP_AUDIO_FEATURE_DRAIN = UINT64_C(1) << 3,
  STBP_AUDIO_FEATURE_COMPRESSED = UINT64_C(1) << 4,
  /* PCM presentation clock is elapsed samples in 90 kHz units, not media PTS. */
  STBP_AUDIO_FEATURE_PCM_SAMPLE_CLOCK = UINT64_C(1) << 5,
  /* DTS shares the AC3 proc mode; downmix requires software decoding to PCM. */
  STBP_AUDIO_FEATURE_DTS_SHARED_AC3 = UINT64_C(1) << 6
};

enum stbp_audio_packet_flag
{
  STBP_AUDIO_PACKET_DISCONTINUITY = UINT32_C(1) << 0,
  STBP_AUDIO_PACKET_END_OF_STREAM = UINT32_C(1) << 1,
  /* PTS belongs to the media timeline, not GUI/keepalive sample timing. */
  STBP_AUDIO_PACKET_MEDIA_PTS = UINT32_C(1) << 2
};

struct stbp_audio_capabilities
{
  uint32_t struct_size;
  uint64_t codec_mask;
  uint64_t feature_mask;
  uint32_t max_pcm_channels;
  uint32_t max_packet_size;
  uint32_t preferred_period_frames;
};

struct stbp_audio_config
{
  uint32_t struct_size;
  enum stbp_audio_codec codec;
  enum stbp_audio_sample_format sample_format;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bits_per_sample;
  const uint8_t* extra_data;
  size_t extra_data_size;
};

struct stbp_audio_packet
{
  uint32_t struct_size;
  const uint8_t* data;
  size_t size;
  int64_t pts_90k;
  int64_t duration_90k;
  uint32_t flags;
};

struct stbp_audio_status
{
  uint32_t struct_size;
  enum stbp_state state;
  enum stbp_result last_error;
  int64_t presentation_pts_90k;
  int64_t queued_end_pts_90k;
  uint64_t packets_queued;
  uint64_t bytes_queued;
};

struct stbp_audio_device;

STBP_EXPORT enum stbp_result stbp_audio_create(const struct stbp_host_callbacks* host,
                                                struct stbp_audio_device** device);
STBP_EXPORT void stbp_audio_destroy(struct stbp_audio_device* device);
STBP_EXPORT enum stbp_result stbp_audio_probe(struct stbp_audio_device* device,
                                               struct stbp_audio_capabilities* capabilities);
STBP_EXPORT enum stbp_result stbp_audio_open(struct stbp_audio_device* device,
                                              const struct stbp_audio_config* config);
STBP_EXPORT enum stbp_result stbp_audio_queue_packet(struct stbp_audio_device* device,
                                                      const struct stbp_audio_packet* packet);
STBP_EXPORT enum stbp_result stbp_audio_get_status(struct stbp_audio_device* device,
                                                    struct stbp_audio_status* status);
STBP_EXPORT enum stbp_result stbp_audio_flush(struct stbp_audio_device* device,
                                               int64_t next_pts_90k);
STBP_EXPORT enum stbp_result stbp_audio_drain(struct stbp_audio_device* device);
STBP_EXPORT enum stbp_result stbp_audio_set_paused(struct stbp_audio_device* device, int paused);
STBP_EXPORT enum stbp_result stbp_audio_set_volume(struct stbp_audio_device* device,
                                                    uint32_t left_percent,
                                                    uint32_t right_percent);
STBP_EXPORT enum stbp_result stbp_audio_close(struct stbp_audio_device* device);

#if defined(__cplusplus)
}
#endif

#endif
