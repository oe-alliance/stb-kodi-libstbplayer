/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stbplayer/loader.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int require_result(enum stbp_result actual, enum stbp_result expected, const char* step)
{
  if (actual == expected)
    return 1;
  fprintf(stderr, "%s: expected %s, got %s\n", step, stbp_result_string(expected),
          stbp_result_string(actual));
  return 0;
}

int main(int argc, char** argv)
{
  struct stbp_loaded_backend* loaded = NULL;
  const struct stbp_backend_api_v1* api;
  struct stbp_host_callbacks host = {0};
  struct stbp_capabilities capabilities = {0};
  struct stbp_stream_config stream = {0};
  struct stbp_clock_config clock = {0};
  struct stbp_packet packet = {0};
  struct stbp_status status = {0};
  uint8_t packet_data[] = {0x00, 0x00, 0x01, 0x09};
  char error[512];
  void* instance = NULL;

  if (argc != 2)
    return 2;
  if (!require_result(stbp_backend_load(argv[1], "../bad", &loaded, error, sizeof(error)),
                      STBP_ERROR_INVALID_ARGUMENT, "reject unsafe name"))
    return 1;
  if (!require_result(stbp_backend_load(argv[1], "fake", &loaded, error, sizeof(error)),
                      STBP_OK, "load"))
    return 1;

  api = stbp_backend_api(loaded);
  host.struct_size = sizeof(host);
  if (!require_result(api->create(&host, &instance), STBP_OK, "create"))
    return 1;

  capabilities.struct_size = sizeof(capabilities);
  if (!require_result(api->probe(instance, &capabilities), STBP_OK, "probe") ||
      !(capabilities.codec_mask & STBP_CODEC_BIT(STBP_CODEC_H264)))
    return 1;

  stream.struct_size = sizeof(stream);
  stream.codec = STBP_CODEC_H264;
  stream.width = 1920;
  stream.height = 1080;
  stream.frame_rate.numerator = 25;
  stream.frame_rate.denominator = 1;
  stream.pixel_aspect.numerator = 1;
  stream.pixel_aspect.denominator = 1;
  clock.struct_size = sizeof(clock);
  clock.initial_pts_90k = 90000;
  clock.kodi_is_clock_master = 1;
  if (!require_result(api->open(instance, &stream, &clock), STBP_OK, "open"))
    return 1;

  packet.struct_size = sizeof(packet);
  packet.data = packet_data;
  packet.size = sizeof(packet_data);
  packet.pts_90k = 180000;
  packet.dts_90k = 180000;
  packet.duration_90k = 3600;
  packet.flags = STBP_PACKET_KEYFRAME;
  if (!require_result(api->queue_packet(instance, &packet), STBP_OK, "queue"))
    return 1;

  status.struct_size = sizeof(status);
  if (!require_result(api->get_status(instance, &status), STBP_OK, "status") ||
      status.presentation_pts_90k != packet.pts_90k || status.packets_queued != 1)
    return 1;
  if (!require_result(api->flush(instance, 270000), STBP_OK, "flush"))
    return 1;
  if (!require_result(api->close(instance), STBP_OK, "close"))
    return 1;

  api->destroy(instance);
  stbp_backend_unload(loaded);
  return 0;
}

