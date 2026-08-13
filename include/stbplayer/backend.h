/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef STBPLAYER_BACKEND_H
#define STBPLAYER_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(_WIN32)
#define STBP_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define STBP_EXPORT __attribute__((visibility("default")))
#else
#define STBP_EXPORT
#endif

#define STBP_ABI_VERSION_1 UINT32_C(0x00010000)
#define STBP_PTS_NONE INT64_MIN

enum stbp_result
{
  STBP_OK = 0,
  STBP_AGAIN = 1,
  STBP_END_OF_STREAM = 2,
  STBP_ERROR_INVALID_ARGUMENT = -1,
  STBP_ERROR_UNSUPPORTED = -2,
  STBP_ERROR_NO_DEVICE = -3,
  STBP_ERROR_BUSY = -4,
  STBP_ERROR_IO = -5,
  STBP_ERROR_BAD_STATE = -6,
  STBP_ERROR_ABI_MISMATCH = -7,
  STBP_ERROR_BACKEND = -8
};

enum stbp_log_level
{
  STBP_LOG_ERROR = 0,
  STBP_LOG_WARNING = 1,
  STBP_LOG_INFO = 2,
  STBP_LOG_DEBUG = 3,
  STBP_LOG_TRACE = 4
};

enum stbp_codec
{
  STBP_CODEC_UNKNOWN = 0,
  STBP_CODEC_MPEG1 = 1,
  STBP_CODEC_MPEG2 = 2,
  STBP_CODEC_MPEG4_PART2 = 3,
  STBP_CODEC_H263 = 4,
  STBP_CODEC_H264 = 5,
  STBP_CODEC_HEVC = 6,
  STBP_CODEC_VC1 = 7,
  STBP_CODEC_WMV3 = 8,
  STBP_CODEC_VP8 = 9,
  STBP_CODEC_VP9 = 10,
  STBP_CODEC_AV1 = 11,
  STBP_CODEC_MJPEG = 12,
  STBP_CODEC_VP6 = 13,
  STBP_CODEC_FLV1 = 14,
  STBP_CODEC_MSMPEG4V3 = 15,
  STBP_CODEC_DIVX4 = 16,
  STBP_CODEC_DIVX5 = 17,
  STBP_CODEC_XVID = 18,
  STBP_CODEC_AVS = 19,
  STBP_CODEC_AVS2 = 20
};

#define STBP_CODEC_BIT(codec) (UINT64_C(1) << (codec))

enum stbp_feature
{
  STBP_FEATURE_PAUSE = UINT64_C(1) << 0,
  STBP_FEATURE_RATE = UINT64_C(1) << 1,
  STBP_FEATURE_REVERSE = UINT64_C(1) << 2,
  STBP_FEATURE_VIDEO_RECT = UINT64_C(1) << 3,
  STBP_FEATURE_VISIBILITY = UINT64_C(1) << 4,
  STBP_FEATURE_PRESENTATION_CLOCK = UINT64_C(1) << 5,
  STBP_FEATURE_DRAIN = UINT64_C(1) << 6,
  STBP_FEATURE_INTERLACED = UINT64_C(1) << 7,
  STBP_FEATURE_HDR10 = UINT64_C(1) << 8,
  STBP_FEATURE_HLG = UINT64_C(1) << 9
};

enum stbp_packet_flag
{
  STBP_PACKET_KEYFRAME = UINT32_C(1) << 0,
  STBP_PACKET_DISCONTINUITY = UINT32_C(1) << 1,
  STBP_PACKET_CODEC_CONFIG = UINT32_C(1) << 2,
  STBP_PACKET_END_OF_STREAM = UINT32_C(1) << 3,
  STBP_PACKET_DROP = UINT32_C(1) << 4,
  /* Kodi found no active audio decoder for this video packet. */
  STBP_PACKET_VIDEO_ONLY = UINT32_C(1) << 5
};

/*
 * STBP_PACKET_DROP is a presentation hint. A backend must still consume the
 * compressed packet unless it can suppress the decoded output without
 * breaking reference-frame state.
 */

enum stbp_stream_flag
{
  STBP_STREAM_INTERLACED = UINT32_C(1) << 0,
  STBP_STREAM_ENCRYPTED = UINT32_C(1) << 1,
  /* H.264/H.265 codec data and packets use Annex-B start codes. */
  STBP_STREAM_ANNEX_B = UINT32_C(1) << 2
};

enum stbp_state
{
  STBP_STATE_CLOSED = 0,
  STBP_STATE_OPEN = 1,
  STBP_STATE_PAUSED = 2,
  STBP_STATE_DRAINING = 3,
  STBP_STATE_ERROR = 4
};

struct stbp_rational
{
  int32_t numerator;
  int32_t denominator;
};

struct stbp_rect
{
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
};

struct stbp_host_callbacks
{
  uint32_t struct_size;
  void* userdata;
  void (*log)(void* userdata, enum stbp_log_level level, const char* message);
  int64_t (*monotonic_time_us)(void* userdata);
};

struct stbp_capabilities
{
  uint32_t struct_size;
  uint64_t codec_mask;
  uint64_t feature_mask;
  uint32_t max_width;
  uint32_t max_height;
  uint32_t max_packet_size;
  uint32_t preferred_queue_bytes;
};

struct stbp_stream_config
{
  uint32_t struct_size;
  enum stbp_codec codec;
  uint32_t codec_profile;
  uint32_t codec_level;
  uint32_t width;
  uint32_t height;
  struct stbp_rational frame_rate;
  struct stbp_rational pixel_aspect;
  uint32_t flags;
  const uint8_t* extra_data;
  size_t extra_data_size;
};

struct stbp_clock_config
{
  uint32_t struct_size;
  int64_t initial_pts_90k;
  int kodi_is_clock_master;
};

struct stbp_packet
{
  uint32_t struct_size;
  const uint8_t* data;
  size_t size;
  int64_t pts_90k;
  int64_t dts_90k;
  int64_t duration_90k;
  uint32_t flags;
};

struct stbp_buffer_state
{
  uint32_t struct_size;
  uint32_t queued_bytes;
  uint32_t capacity_bytes;
  uint32_t queued_packets;
  int can_accept_packet;
};

struct stbp_status
{
  uint32_t struct_size;
  enum stbp_state state;
  enum stbp_result last_error;
  int64_t presentation_pts_90k;
  uint64_t packets_queued;
  uint64_t packets_dropped;
};

struct stbp_backend_api_v1
{
  uint32_t abi_version;
  uint32_t struct_size;
  const char* backend_name;
  const char* backend_version;

  enum stbp_result (*create)(const struct stbp_host_callbacks* host, void** instance);
  void (*destroy)(void* instance);
  enum stbp_result (*probe)(void* instance, struct stbp_capabilities* capabilities);
  enum stbp_result (*open)(void* instance,
                           const struct stbp_stream_config* stream,
                           const struct stbp_clock_config* clock);
  enum stbp_result (*queue_packet)(void* instance, const struct stbp_packet* packet);
  enum stbp_result (*get_buffer_state)(void* instance, struct stbp_buffer_state* state);
  enum stbp_result (*get_status)(void* instance, struct stbp_status* status);
  enum stbp_result (*flush)(void* instance, int64_t next_pts_90k);
  enum stbp_result (*drain)(void* instance);
  enum stbp_result (*reset)(void* instance);
  enum stbp_result (*set_speed)(void* instance, struct stbp_rational speed);
  enum stbp_result (*set_paused)(void* instance, int paused);
  enum stbp_result (*set_video_rect)(void* instance,
                                     const struct stbp_rect* source,
                                     const struct stbp_rect* destination);
  enum stbp_result (*set_visible)(void* instance, int visible);
  enum stbp_result (*close)(void* instance);
};

typedef const struct stbp_backend_api_v1* (*stbp_backend_get_api_fn)(
    uint32_t host_abi_version, uint32_t host_api_size);

STBP_EXPORT const struct stbp_backend_api_v1* stbp_backend_get_api(
    uint32_t host_abi_version, uint32_t host_api_size);

#if defined(__cplusplus)
}
#endif

#endif
