/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stbplayer/backend.h"

#include <stdlib.h>
#include <string.h>

struct fake_instance
{
  struct stbp_host_callbacks host;
  struct stbp_status status;
  int visible;
};

static enum stbp_result fake_create(const struct stbp_host_callbacks* host, void** instance)
{
  struct fake_instance* fake;

  if (host == NULL || host->struct_size < sizeof(*host) || instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  fake = (struct fake_instance*)calloc(1, sizeof(*fake));
  if (fake == NULL)
    return STBP_ERROR_BACKEND;
  fake->host = *host;
  fake->status.struct_size = sizeof(fake->status);
  fake->status.state = STBP_STATE_CLOSED;
  fake->status.presentation_pts_90k = STBP_PTS_NONE;
  *instance = fake;
  return STBP_OK;
}

static void fake_destroy(void* instance)
{
  free(instance);
}

static enum stbp_result fake_probe(void* instance, struct stbp_capabilities* capabilities)
{
  if (instance == NULL || capabilities == NULL ||
      capabilities->struct_size < sizeof(*capabilities))
    return STBP_ERROR_INVALID_ARGUMENT;
  capabilities->codec_mask = STBP_CODEC_BIT(STBP_CODEC_H264) | STBP_CODEC_BIT(STBP_CODEC_HEVC);
  capabilities->feature_mask = STBP_FEATURE_PAUSE | STBP_FEATURE_RATE |
                               STBP_FEATURE_VIDEO_RECT | STBP_FEATURE_VISIBILITY |
                               STBP_FEATURE_PRESENTATION_CLOCK | STBP_FEATURE_DRAIN;
  capabilities->max_width = 3840;
  capabilities->max_height = 2160;
  capabilities->max_packet_size = 4U * 1024U * 1024U;
  capabilities->preferred_queue_bytes = 8U * 1024U * 1024U;
  return STBP_OK;
}

static enum stbp_result fake_open(void* instance,
                                  const struct stbp_stream_config* stream,
                                  const struct stbp_clock_config* clock)
{
  struct fake_instance* fake = (struct fake_instance*)instance;
  (void)clock;
  if (fake == NULL || stream == NULL || stream->struct_size < sizeof(*stream))
    return STBP_ERROR_INVALID_ARGUMENT;
  if (stream->flags & STBP_STREAM_ENCRYPTED)
    return STBP_ERROR_UNSUPPORTED;
  if (stream->codec != STBP_CODEC_H264 && stream->codec != STBP_CODEC_HEVC)
    return STBP_ERROR_UNSUPPORTED;
  if (fake->status.state != STBP_STATE_CLOSED)
    return STBP_ERROR_BAD_STATE;
  fake->status.state = STBP_STATE_OPEN;
  fake->status.last_error = STBP_OK;
  return STBP_OK;
}

static enum stbp_result fake_queue_packet(void* instance, const struct stbp_packet* packet)
{
  struct fake_instance* fake = (struct fake_instance*)instance;
  if (fake == NULL || packet == NULL || packet->struct_size < sizeof(*packet) ||
      (packet->data == NULL && packet->size != 0))
    return STBP_ERROR_INVALID_ARGUMENT;
  if (fake->status.state != STBP_STATE_OPEN && fake->status.state != STBP_STATE_PAUSED)
    return STBP_ERROR_BAD_STATE;
  if (packet->size > 4U * 1024U * 1024U)
    return STBP_AGAIN;
  ++fake->status.packets_queued;
  if (packet->flags & STBP_PACKET_DROP)
    ++fake->status.packets_dropped;
  if (packet->pts_90k != STBP_PTS_NONE)
    fake->status.presentation_pts_90k = packet->pts_90k;
  return STBP_OK;
}

static enum stbp_result fake_get_buffer_state(void* instance, struct stbp_buffer_state* state)
{
  if (instance == NULL || state == NULL || state->struct_size < sizeof(*state))
    return STBP_ERROR_INVALID_ARGUMENT;
  state->queued_bytes = 0;
  state->capacity_bytes = 8U * 1024U * 1024U;
  state->queued_packets = 0;
  state->can_accept_packet = 1;
  return STBP_OK;
}

static enum stbp_result fake_get_status(void* instance, struct stbp_status* status)
{
  struct fake_instance* fake = (struct fake_instance*)instance;
  uint32_t requested_size;
  if (fake == NULL || status == NULL || status->struct_size < sizeof(*status))
    return STBP_ERROR_INVALID_ARGUMENT;
  requested_size = status->struct_size;
  *status = fake->status;
  status->struct_size = requested_size;
  return STBP_OK;
}

static enum stbp_result fake_flush(void* instance, int64_t next_pts_90k)
{
  struct fake_instance* fake = (struct fake_instance*)instance;
  if (fake == NULL || fake->status.state == STBP_STATE_CLOSED)
    return STBP_ERROR_BAD_STATE;
  fake->status.presentation_pts_90k = next_pts_90k;
  return STBP_OK;
}

static enum stbp_result fake_drain(void* instance)
{
  struct fake_instance* fake = (struct fake_instance*)instance;
  if (fake == NULL || fake->status.state == STBP_STATE_CLOSED)
    return STBP_ERROR_BAD_STATE;
  fake->status.state = STBP_STATE_DRAINING;
  return STBP_OK;
}

static enum stbp_result fake_reset(void* instance)
{
  struct fake_instance* fake = (struct fake_instance*)instance;
  if (fake == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  fake->status.state = STBP_STATE_CLOSED;
  fake->status.presentation_pts_90k = STBP_PTS_NONE;
  return STBP_OK;
}

static enum stbp_result fake_set_speed(void* instance, struct stbp_rational speed)
{
  if (instance == NULL || speed.denominator == 0)
    return STBP_ERROR_INVALID_ARGUMENT;
  return STBP_OK;
}

static enum stbp_result fake_set_paused(void* instance, int paused)
{
  struct fake_instance* fake = (struct fake_instance*)instance;
  if (fake == NULL || fake->status.state == STBP_STATE_CLOSED)
    return STBP_ERROR_BAD_STATE;
  fake->status.state = paused ? STBP_STATE_PAUSED : STBP_STATE_OPEN;
  return STBP_OK;
}

static enum stbp_result fake_set_video_rect(void* instance,
                                            const struct stbp_rect* source,
                                            const struct stbp_rect* destination)
{
  if (instance == NULL || source == NULL || destination == NULL || source->width <= 0 ||
      source->height <= 0 || destination->width <= 0 || destination->height <= 0)
    return STBP_ERROR_INVALID_ARGUMENT;
  return STBP_OK;
}

static enum stbp_result fake_set_visible(void* instance, int visible)
{
  struct fake_instance* fake = (struct fake_instance*)instance;
  if (fake == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  fake->visible = visible != 0;
  return STBP_OK;
}

static enum stbp_result fake_close(void* instance)
{
  return fake_reset(instance);
}

static const struct stbp_backend_api_v1 fake_api = {
    STBP_ABI_VERSION_1,
    sizeof(struct stbp_backend_api_v1),
    "fake",
    "0.1.0",
    fake_create,
    fake_destroy,
    fake_probe,
    fake_open,
    fake_queue_packet,
    fake_get_buffer_state,
    fake_get_status,
    fake_flush,
    fake_drain,
    fake_reset,
    fake_set_speed,
    fake_set_paused,
    fake_set_video_rect,
    fake_set_visible,
    fake_close};

STBP_EXPORT const struct stbp_backend_api_v1* stbp_backend_get_api(uint32_t host_abi_version,
                                                                   uint32_t host_api_size)
{
  if (host_abi_version != STBP_ABI_VERSION_1 || host_api_size < sizeof(fake_api))
    return NULL;
  return &fake_api;
}

