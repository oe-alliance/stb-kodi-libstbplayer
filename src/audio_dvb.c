/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stbplayer/audio.h"

#include "audio_pes.h"
#ifndef STBP_DVB_AUDIO_HISI_PCM_TRACK
#define STBP_DVB_AUDIO_HISI_PCM_TRACK 0
#endif
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
#include "hisi_pcm.h"
#endif
#if STBP_DVB_AUDIO_HISI_VIDEO_CLOCK
#include "hisi_audio_clock.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/dvb/audio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef AUDIO_GET_PTS
#define AUDIO_GET_PTS _IOR('o', 19, __u64)
#endif

#ifndef STBP_DVB_AUDIO_DEVICE
#define STBP_DVB_AUDIO_DEVICE "/dev/dvb/adapter0/audio0"
#endif

#ifndef STBP_DVB_AUDIO_VARIANT
#define STBP_DVB_AUDIO_VARIANT STBP_AUDIO_PES_NORMAL
#endif

#ifndef STBP_DVB_AUDIO_HAVE_EAC3
#define STBP_DVB_AUDIO_HAVE_EAC3 1
#endif

#ifndef STBP_DVB_AUDIO_HAVE_DTS
#define STBP_DVB_AUDIO_HAVE_DTS 1
#endif

#ifndef STBP_DVB_AUDIO_PCM_SAMPLE_CLOCK
#define STBP_DVB_AUDIO_PCM_SAMPLE_CLOCK 0
#endif

#ifndef STBP_DVB_AUDIO_DTS_SHARED_AC3
#define STBP_DVB_AUDIO_DTS_SHARED_AC3 0
#endif

#define AUDIO_WRITE_TIMEOUT_MS 25
#define AUDIO_DRAIN_POLLS 40
#define AUDIO_DRAIN_POLL_MS 25

struct stbp_audio_device
{
  struct stbp_host_callbacks host;
  struct stbp_audio_config config;
  struct stbp_audio_status status;
  int fd;
  uint8_t* pending;
  size_t pending_size;
  size_t pending_offset;
  size_t pending_header_size;
  int video_clock_logged;
  int media_pts;
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  struct stbp_hisi_pcm* pcm;
#endif
};

static void audio_log(struct stbp_audio_device* device,
                      enum stbp_log_level level,
                      const char* message)
{
  if (device->host.log != NULL)
    device->host.log(device->host.userdata, level, message);
}

static void audio_log_errno(struct stbp_audio_device* device,
                            enum stbp_log_level level,
                            const char* operation)
{
  char message[256];

  (void)snprintf(message, sizeof(message), "%s: %s", operation, strerror(errno));
  audio_log(device, level, message);
}

static int config_is_valid(const struct stbp_audio_config* config)
{
  if (config == NULL || config->struct_size < sizeof(*config) || config->sample_rate == 0 ||
      config->channels == 0)
    return 0;
  if (stbp_audio_codec_bypass(config->codec, STBP_DVB_AUDIO_VARIANT) < 0)
    return 0;
  if ((!STBP_DVB_AUDIO_HAVE_EAC3 && config->codec == STBP_AUDIO_CODEC_EAC3) ||
      (!STBP_DVB_AUDIO_HAVE_DTS && config->codec == STBP_AUDIO_CODEC_DTS))
    return 0;
  if (config->codec == STBP_AUDIO_CODEC_PCM &&
      (config->sample_format != STBP_AUDIO_SAMPLE_S16_NATIVE || config->bits_per_sample != 16U ||
       config->channels > 2U))
    return 0;
  return 1;
}

static void release_pending(struct stbp_audio_device* device)
{
  free(device->pending);
  device->pending = NULL;
  device->pending_size = 0;
  device->pending_offset = 0;
  device->pending_header_size = 0;
}

static enum stbp_result write_pending(struct stbp_audio_device* device)
{
  while (device->pending_offset < device->pending_size)
  {
    struct pollfd descriptor = {device->fd, POLLOUT, 0};
    const int poll_result = poll(&descriptor, 1, AUDIO_WRITE_TIMEOUT_MS);
    const size_t write_end = device->pending_offset < device->pending_header_size
                                 ? device->pending_header_size
                                 : device->pending_size;
    ssize_t written;

    if (poll_result == 0)
      return STBP_AGAIN;
    if (poll_result < 0)
    {
      if (errno == EINTR)
        continue;
      audio_log_errno(device, STBP_LOG_ERROR, "polling DVB audio device");
      return STBP_ERROR_IO;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      return STBP_ERROR_IO;
    if ((descriptor.revents & POLLOUT) == 0)
      continue;

    /* Vendor PCM parsers require the PES header and payload in separate writes,
     * matching dvbaudiosink. Preserve that boundary even after a short write. */
    written = write(device->fd, device->pending + device->pending_offset,
                    write_end - device->pending_offset);
    if (written < 0)
    {
      if (errno == EAGAIN)
        return STBP_AGAIN;
      if (errno == EINTR)
        continue;
      audio_log_errno(device, STBP_LOG_ERROR, "writing DVB audio packet");
      return STBP_ERROR_IO;
    }
    if (written == 0)
      return STBP_ERROR_IO;
    device->pending_offset += (size_t)written;
  }
  release_pending(device);
  return STBP_OK;
}

enum stbp_result stbp_audio_create(const struct stbp_host_callbacks* host,
                                   struct stbp_audio_device** device)
{
  struct stbp_audio_device* created;

  if (host == NULL || host->struct_size < sizeof(*host) || device == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  created = (struct stbp_audio_device*)calloc(1, sizeof(*created));
  if (created == NULL)
    return STBP_ERROR_BACKEND;
  created->host = *host;
  created->fd = -1;
  created->status.struct_size = sizeof(created->status);
  created->status.state = STBP_STATE_CLOSED;
  created->status.presentation_pts_90k = STBP_AUDIO_PTS_NONE;
  created->status.queued_end_pts_90k = STBP_AUDIO_PTS_NONE;
  *device = created;
  return STBP_OK;
}

void stbp_audio_destroy(struct stbp_audio_device* device)
{
  if (device == NULL)
    return;
  (void)stbp_audio_close(device);
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  stbp_hisi_pcm_unselect(device);
#endif
  free(device);
}

enum stbp_result stbp_audio_probe(struct stbp_audio_device* device,
                                  struct stbp_audio_capabilities* capabilities)
{
  if (device == NULL || capabilities == NULL ||
      capabilities->struct_size < sizeof(*capabilities))
    return STBP_ERROR_INVALID_ARGUMENT;

  capabilities->codec_mask = STBP_AUDIO_CODEC_BIT(STBP_AUDIO_CODEC_PCM) |
                             STBP_AUDIO_CODEC_BIT(STBP_AUDIO_CODEC_MPEG) |
                             STBP_AUDIO_CODEC_BIT(STBP_AUDIO_CODEC_MP3) |
                             STBP_AUDIO_CODEC_BIT(STBP_AUDIO_CODEC_AAC) |
                             STBP_AUDIO_CODEC_BIT(STBP_AUDIO_CODEC_AC3);
  if (STBP_DVB_AUDIO_HAVE_EAC3)
    capabilities->codec_mask |= STBP_AUDIO_CODEC_BIT(STBP_AUDIO_CODEC_EAC3);
  if (STBP_DVB_AUDIO_HAVE_DTS)
    capabilities->codec_mask |= STBP_AUDIO_CODEC_BIT(STBP_AUDIO_CODEC_DTS);
  capabilities->feature_mask = STBP_AUDIO_FEATURE_HW_AV_SYNC |
                               STBP_AUDIO_FEATURE_PRESENTATION_CLOCK |
                               STBP_AUDIO_FEATURE_PAUSE | STBP_AUDIO_FEATURE_DRAIN |
                               STBP_AUDIO_FEATURE_COMPRESSED;
  if (STBP_DVB_AUDIO_PCM_SAMPLE_CLOCK || STBP_DVB_AUDIO_HISI_PCM_TRACK)
    capabilities->feature_mask |= STBP_AUDIO_FEATURE_PCM_SAMPLE_CLOCK;
  if (STBP_DVB_AUDIO_HAVE_DTS && STBP_DVB_AUDIO_DTS_SHARED_AC3)
    capabilities->feature_mask |= STBP_AUDIO_FEATURE_DTS_SHARED_AC3;
  capabilities->max_pcm_channels = 2;
  capabilities->max_packet_size = UINT16_MAX - STBP_AUDIO_PES_HEADER_MAX;
  capabilities->preferred_period_frames = 2048;
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  capabilities->preferred_period_frames = 480;
#endif
  return access(STBP_DVB_AUDIO_DEVICE, R_OK | W_OK) == 0 ? STBP_OK : STBP_ERROR_NO_DEVICE;
}

enum stbp_result stbp_audio_open(struct stbp_audio_device* device,
                                 const struct stbp_audio_config* config)
{
  const int bypass = config != NULL
                         ? stbp_audio_codec_bypass(config->codec, STBP_DVB_AUDIO_VARIANT)
                         : -1;

  if (device == NULL || !config_is_valid(config))
    return STBP_ERROR_INVALID_ARGUMENT;
  if (device->status.state != STBP_STATE_CLOSED)
    return STBP_ERROR_BAD_STATE;

#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  if (config->codec == STBP_AUDIO_CODEC_PCM)
  {
    const enum stbp_result result = stbp_hisi_pcm_open(&device->host, config, &device->pcm);
    if (result != STBP_OK)
      return result;
    stbp_hisi_pcm_select(device, 1);
    device->config = *config;
    memset(&device->status, 0, sizeof(device->status));
    device->status.struct_size = sizeof(device->status);
    device->status.state = STBP_STATE_OPEN;
    device->status.queued_end_pts_90k = STBP_AUDIO_PTS_NONE;
    audio_log(device, STBP_LOG_INFO, "HiSilicon native PCM slave track opened (sample clock)");
    return STBP_OK;
  }
#endif
  device->fd = open(STBP_DVB_AUDIO_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (device->fd < 0)
  {
    const int open_error = errno;
    audio_log_errno(device, STBP_LOG_ERROR, "opening DVB audio device");
    return open_error == ENOENT ? STBP_ERROR_NO_DEVICE
                               : (open_error == EBUSY ? STBP_ERROR_BUSY : STBP_ERROR_IO);
  }

  if (ioctl(device->fd, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_MEMORY) < 0 ||
      ioctl(device->fd, AUDIO_PAUSE) < 0 || ioctl(device->fd, AUDIO_SET_AV_SYNC, 1) < 0 ||
      ioctl(device->fd, AUDIO_SET_BYPASS_MODE, bypass) < 0 ||
      ioctl(device->fd, AUDIO_PLAY) < 0 || ioctl(device->fd, AUDIO_CONTINUE) < 0)
  {
    audio_log_errno(device, STBP_LOG_ERROR, "configuring DVB audio decoder");
    (void)stbp_audio_close(device);
    return STBP_ERROR_IO;
  }

#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  stbp_hisi_pcm_select(device, 0);
#endif
  device->config = *config;
  device->status.state = STBP_STATE_OPEN;
  device->status.last_error = STBP_OK;
  device->status.presentation_pts_90k = STBP_AUDIO_PTS_NONE;
  device->status.queued_end_pts_90k = STBP_AUDIO_PTS_NONE;
  device->status.packets_queued = 0;
  device->status.bytes_queued = 0;
  device->video_clock_logged = 0;
  device->media_pts = 0;
  audio_log(device, STBP_LOG_INFO, "DVB hardware audio decoder opened with AV sync enabled");
  return STBP_OK;
}

enum stbp_result stbp_audio_queue_packet(struct stbp_audio_device* device,
                                         const struct stbp_audio_packet* packet)
{
  uint8_t header[STBP_AUDIO_PES_HEADER_MAX];
  size_t header_size;
  size_t output_size;
  enum stbp_result result;

  if (device == NULL || packet == NULL || packet->struct_size < sizeof(*packet) ||
      packet->data == NULL || packet->size == 0)
    return STBP_ERROR_INVALID_ARGUMENT;
  if (device->status.state != STBP_STATE_OPEN && device->status.state != STBP_STATE_PAUSED)
    return STBP_ERROR_BAD_STATE;

  if ((packet->flags & STBP_AUDIO_PACKET_DISCONTINUITY) != 0)
  {
    result = stbp_audio_flush(device, STBP_AUDIO_PTS_NONE);
    if (result != STBP_OK)
      return result;
  }
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  if (device->pcm != NULL)
  {
    result = stbp_hisi_pcm_queue(device->pcm, packet);
    if (result != STBP_OK)
      return result;
    ++device->status.packets_queued;
    device->status.bytes_queued += packet->size;
    device->status.queued_end_pts_90k = (int64_t)(device->status.bytes_queued /
        (device->config.channels * sizeof(int16_t)) * 90000 / device->config.sample_rate);
    return STBP_OK;
  }
#endif
  result = write_pending(device);
  if (result != STBP_OK)
    return result;

  header_size = stbp_audio_make_pes_header(header, sizeof(header), &device->config,
                                           STBP_DVB_AUDIO_VARIANT, packet->size,
                                           packet->pts_90k);
  if (header_size == 0 || packet->size > SIZE_MAX - header_size)
    return STBP_ERROR_INVALID_ARGUMENT;
  output_size = header_size + packet->size;
  device->pending = (uint8_t*)malloc(output_size);
  if (device->pending == NULL)
    return STBP_ERROR_BACKEND;
  device->pending_size = output_size;
  device->pending_offset = 0;
  device->pending_header_size = header_size;
  if (STBP_DVB_AUDIO_VARIANT == STBP_AUDIO_PES_DREAMBOX &&
      device->config.codec == STBP_AUDIO_CODEC_DTS)
  {
    const size_t payload_size = stbp_audio_normalize_dts(
        device->pending + header_size, packet->size, packet->data, packet->size);
    if (payload_size == 0 || stbp_audio_make_pes_header(
            header, sizeof(header), &device->config, STBP_DVB_AUDIO_VARIANT,
            payload_size, packet->pts_90k) != header_size)
    {
      release_pending(device);
      return STBP_ERROR_INVALID_ARGUMENT;
    }
    device->pending_size = header_size + payload_size;
  }
  else
    memcpy(device->pending + header_size, packet->data, packet->size);
  memcpy(device->pending, header, header_size);
  result = write_pending(device);

  if (result != STBP_OK && result != STBP_AGAIN)
  {
    device->status.last_error = result;
    return result;
  }

  /* Ownership transfers once the full packet is buffered. A short write is
   * completed before accepting the next packet, without replaying its PES header. */
  device->status.last_error = STBP_OK;

  ++device->status.packets_queued;
  device->status.bytes_queued += packet->size;
  device->media_pts = (packet->flags & STBP_AUDIO_PACKET_MEDIA_PTS) != 0;
  if (packet->pts_90k != STBP_AUDIO_PTS_NONE)
  {
    device->status.queued_end_pts_90k = packet->pts_90k;
    if (packet->duration_90k > 0)
      device->status.queued_end_pts_90k += packet->duration_90k;
  }
  return STBP_OK;
}

static int64_t audio_presentation_pts(struct stbp_audio_device* device)
{
  uint64_t decoder_pts = 0;
  int64_t result = STBP_AUDIO_PTS_NONE;
  if (device->fd < 0)
    return result;
  if (ioctl(device->fd, AUDIO_GET_PTS, &decoder_pts) == 0)
    result = (int64_t)decoder_pts;
#if STBP_DVB_AUDIO_HISI_VIDEO_CLOCK
  /* These HiSi drivers can retain AUDIO_GET_PTS after a seek even while
   * audio plays. Their video PTS remains usable on the shared media timeline. */
  if (device->media_pts && !STBP_DVB_AUDIO_PCM_SAMPLE_CLOCK &&
      device->status.queued_end_pts_90k != STBP_AUDIO_PTS_NONE)
  {
    const int64_t video_pts = stbp_hisi_audio_clock_read();
    if (video_pts != STBP_AUDIO_PTS_NONE)
    {
      int64_t distance = (int64_t)(((uint64_t)video_pts -
          (uint64_t)device->status.queued_end_pts_90k) & UINT64_C(0x1ffffffff));
      if (distance >= INT64_C(0x100000000))
        distance -= INT64_C(0x200000000);
      /* Do not use a video PTS left on the old timeline during a seek. */
      if (distance >= -900000 && distance <= 900000)
      {
        result = video_pts;
        if (!device->video_clock_logged)
        {
          audio_log(device, STBP_LOG_INFO, "HiSilicon media audio uses video presentation clock");
          device->video_clock_logged = 1;
        }
      }
    }
  }
#endif
  return result;
}

enum stbp_result stbp_audio_get_status(struct stbp_audio_device* device,
                                       struct stbp_audio_status* status)
{
  uint32_t requested_size;

  if (device == NULL || status == NULL || status->struct_size < sizeof(*status))
    return STBP_ERROR_INVALID_ARGUMENT;
  requested_size = status->struct_size;
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  if (device->pcm != NULL)
  {
    const enum stbp_result result = stbp_hisi_pcm_pts(device->pcm,
        &device->status.presentation_pts_90k);
    if (result != STBP_OK)
      return result;
  }
  else
#endif
  device->status.presentation_pts_90k = audio_presentation_pts(device);
  *status = device->status;
  status->struct_size = requested_size;
  return STBP_OK;
}

enum stbp_result stbp_audio_flush(struct stbp_audio_device* device, int64_t next_pts_90k)
{
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  if (device != NULL && device->pcm != NULL)
  {
    const enum stbp_result result = stbp_hisi_pcm_flush(device->pcm);
    if (result != STBP_OK)
      return result;
    device->status.presentation_pts_90k = 0;
    device->status.queued_end_pts_90k = STBP_AUDIO_PTS_NONE;
    device->status.bytes_queued = 0;
    device->status.packets_queued = 0;
    return STBP_OK;
  }
#endif
  if (device == NULL || device->fd < 0 || device->status.state == STBP_STATE_CLOSED)
    return STBP_ERROR_BAD_STATE;
  release_pending(device);
  if (ioctl(device->fd, AUDIO_CLEAR_BUFFER) < 0)
  {
    audio_log_errno(device, STBP_LOG_WARNING, "clearing DVB audio buffer");
    return STBP_ERROR_IO;
  }
  device->status.presentation_pts_90k = STBP_AUDIO_PTS_NONE;
  device->status.queued_end_pts_90k = next_pts_90k;
  device->media_pts = 0;
  return STBP_OK;
}

enum stbp_result stbp_audio_drain(struct stbp_audio_device* device)
{
  const struct timespec interval = {0, AUDIO_DRAIN_POLL_MS * 1000000L};
  enum stbp_result result = STBP_AGAIN;
  int attempt;

#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  if (device != NULL && device->pcm != NULL)
    return stbp_hisi_pcm_drain(device->pcm);
#endif
  if (device == NULL || device->fd < 0 || device->status.state == STBP_STATE_CLOSED)
    return STBP_ERROR_BAD_STATE;
  device->status.state = STBP_STATE_DRAINING;
  for (attempt = 0; attempt < AUDIO_DRAIN_POLLS; ++attempt)
  {
    int64_t decoder_pts;
    result = write_pending(device);
    if (result != STBP_OK && result != STBP_AGAIN)
      break;
    decoder_pts = result == STBP_OK ? audio_presentation_pts(device) : STBP_AUDIO_PTS_NONE;
    if (result == STBP_OK && decoder_pts != STBP_AUDIO_PTS_NONE)
    {
      int64_t distance;
      device->status.presentation_pts_90k = (int64_t)decoder_pts;
      distance = (device->status.queued_end_pts_90k - (int64_t)decoder_pts) &
                 INT64_C(0x1ffffffff);
      if (distance >= INT64_C(0x100000000))
        distance -= INT64_C(0x200000000);
      if (device->status.queued_end_pts_90k != STBP_AUDIO_PTS_NONE && distance <= 900)
        break;
    }
    result = STBP_AGAIN;
    (void)nanosleep(&interval, NULL);
  }
  device->status.state = STBP_STATE_OPEN;
  return result;
}

enum stbp_result stbp_audio_set_paused(struct stbp_audio_device* device, int paused)
{
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  if (device != NULL && device->pcm != NULL)
  {
    const enum stbp_result result = stbp_hisi_pcm_pause(device->pcm, paused);
    if (result == STBP_OK)
      device->status.state = paused ? STBP_STATE_PAUSED : STBP_STATE_OPEN;
    return result;
  }
#endif
  if (device == NULL || device->fd < 0 || device->status.state == STBP_STATE_CLOSED)
    return STBP_ERROR_BAD_STATE;
  if (ioctl(device->fd, paused ? AUDIO_PAUSE : AUDIO_CONTINUE) < 0)
  {
    audio_log_errno(device, STBP_LOG_ERROR, paused ? "pausing DVB audio" : "resuming DVB audio");
    return STBP_ERROR_IO;
  }
  device->status.state = paused ? STBP_STATE_PAUSED : STBP_STATE_OPEN;
  return STBP_OK;
}

enum stbp_result stbp_audio_set_volume(struct stbp_audio_device* device,
                                       uint32_t left_percent,
                                       uint32_t right_percent)
{
  audio_mixer_t mixer;
  int muted;

  if (device == NULL || device->status.state == STBP_STATE_CLOSED)
    return STBP_ERROR_BAD_STATE;
  if (left_percent > 100U || right_percent > 100U)
    return STBP_ERROR_INVALID_ARGUMENT;
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  if (device != NULL && device->pcm != NULL)
    return stbp_hisi_pcm_volume(device->pcm, left_percent, right_percent);
#endif
  if (device->fd < 0)
    return STBP_ERROR_BAD_STATE;

  /* Linux-DVB uses attenuation: zero is full volume and 63 is silent. */
  mixer.volume_left = 63U - left_percent * 63U / 100U;
  mixer.volume_right = 63U - right_percent * 63U / 100U;
  if (ioctl(device->fd, AUDIO_SET_MIXER, &mixer) < 0)
  {
    audio_log_errno(device, STBP_LOG_WARNING, "setting DVB audio volume");
    return STBP_ERROR_IO;
  }

  muted = left_percent == 0U && right_percent == 0U;
  if (ioctl(device->fd, AUDIO_SET_MUTE, muted) < 0)
  {
    audio_log_errno(device, STBP_LOG_WARNING, "setting DVB audio mute");
    return STBP_ERROR_IO;
  }
  return STBP_OK;
}

enum stbp_result stbp_audio_close(struct stbp_audio_device* device)
{
  if (device == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
#if STBP_DVB_AUDIO_HISI_PCM_TRACK
  stbp_hisi_pcm_close(device->pcm);
  device->pcm = NULL;
#endif
  release_pending(device);
  if (device->fd >= 0)
  {
    (void)ioctl(device->fd, AUDIO_STOP);
    (void)ioctl(device->fd, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX);
    (void)ioctl(device->fd, AUDIO_CLEAR_BUFFER);
    close(device->fd);
    device->fd = -1;
  }
  memset(&device->config, 0, sizeof(device->config));
  device->media_pts = 0;
  device->status.state = STBP_STATE_CLOSED;
  device->status.presentation_pts_90k = STBP_AUDIO_PTS_NONE;
  device->status.queued_end_pts_90k = STBP_AUDIO_PTS_NONE;
  return STBP_OK;
}
