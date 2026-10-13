/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hisi_pcm.h"

#include <dlfcn.h>
#include <linux/dvb/video.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

/* A seek recreates the PCM track while the video thread flushes concurrently.
 * Retain selection across close/open; release it when its audio device dies. */
static atomic_uintptr_t selected_device;

void stbp_hisi_pcm_select(const void* device, int enabled)
{
  atomic_store(&selected_device, enabled ? (uintptr_t)device : 0);
}

void stbp_hisi_pcm_unselect(const void* device)
{
  uintptr_t expected = (uintptr_t)device;
  (void)atomic_compare_exchange_strong(&selected_device, &expected, 0);
}

enum stbp_result stbp_hisi_pcm_flush_video(int video_fd)
{
  if (atomic_load(&selected_device) == 0)
    return STBP_ERROR_UNSUPPORTED;
  if (video_fd < 0)
    return STBP_ERROR_INVALID_ARGUMENT;
  /* CLEAR_BUFFER alone leaves the first target frame frozen when the audio
   * clock is supplied by a separate native PCM track, not the DVB decoder. */
  if (ioctl(video_fd, VIDEO_STOP, 0) < 0 ||
      ioctl(video_fd, VIDEO_CLEAR_BUFFER) < 0 ||
      ioctl(video_fd, VIDEO_PLAY) < 0 || ioctl(video_fd, VIDEO_CONTINUE) < 0)
    return STBP_ERROR_IO;
  return STBP_OK;
}

/* 32-bit HiSi SDK layouts, verified against the receiver library. Kodi's
 * existing HiSi starter preloads libhi_common/libhi_msp; no vendor library or
 * E2 dependency is added to the build. Only PCM uses this private slave track. */
struct hi_pcm_track_attr
{
  uint32_t type, fade_in_ms, fade_out_ms, output_bytes, level_ms, threshold_ms;
};
struct hi_pcm_frame
{
  int32_t bits, interleaved;
  uint32_t rate, channels, pts_ms;
  int32_t* pcm;
  int32_t* bitstream;
  uint32_t samples, bitstream_bytes, index, iec_type;
};
struct hi_pcm_gain { int32_t linear, left, right; };
_Static_assert(sizeof(struct hi_pcm_track_attr) == 24, "HiSi track ABI");
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(struct hi_pcm_frame) == 44, "HiSi frame ABI");
#endif

struct stbp_hisi_pcm
{
  struct stbp_host_callbacks host;
  struct stbp_audio_config config;
  uint32_t track;
  uint32_t index;
  uint64_t frames;
  int64_t played;
  int initialized;
  int paused;
  int (*init)(void);
  int (*deinit)(void);
  int (*default_attr)(int, struct hi_pcm_track_attr*);
  int (*create)(int, const struct hi_pcm_track_attr*, uint32_t*);
  int (*destroy)(uint32_t);
  int (*send)(uint32_t, const struct hi_pcm_frame*);
  int (*delay)(uint32_t, uint32_t*);
  int (*flush)(uint32_t);
  int (*pause)(uint32_t);
  int (*start)(uint32_t);
  int (*volume)(uint32_t, const struct hi_pcm_gain*);
};

static enum stbp_result pcm_error(struct stbp_hisi_pcm* pcm, const char* operation, int rc)
{
  if (pcm->host.log != NULL)
  {
    char message[160];
    (void)snprintf(message, sizeof(message), "HiSi PCM %s failed: 0x%08x", operation, (unsigned)rc);
    pcm->host.log(pcm->host.userdata, STBP_LOG_ERROR, message);
  }
  return STBP_ERROR_IO;
}

enum stbp_result stbp_hisi_pcm_open(const struct stbp_host_callbacks* host,
    const struct stbp_audio_config* config, struct stbp_hisi_pcm** output)
{
  struct stbp_hisi_pcm* pcm = calloc(1, sizeof(*pcm));
  struct hi_pcm_track_attr attr = {0};
  int rc;
  if (pcm == NULL)
    return STBP_ERROR_BACKEND;
  *output = NULL;
  pcm->host = *host;
  pcm->config = *config;
  pcm->track = UINT32_MAX;
#define LOAD(field, symbol) do { \
  void* address = dlsym(RTLD_DEFAULT, symbol); \
  _Static_assert(sizeof(pcm->field) == sizeof(address), "POSIX function pointer ABI"); \
  memcpy(&pcm->field, &address, sizeof(address)); \
  if (pcm->field == NULL) { \
    pcm_error(pcm, "missing " symbol, 0); \
    free(pcm); return STBP_ERROR_NO_DEVICE; \
  } \
} while (0)
  LOAD(init, "HI_UNF_SND_Init");
  LOAD(deinit, "HI_UNF_SND_DeInit");
  LOAD(default_attr, "HI_UNF_SND_GetDefaultTrackAttr");
  LOAD(create, "HI_UNF_SND_CreateTrack");
  LOAD(destroy, "HI_UNF_SND_DestroyTrack");
  LOAD(send, "HI_UNF_SND_SendTrackData");
  LOAD(delay, "HI_UNF_SND_GetTrackDelayMs");
  LOAD(flush, "HI_MPI_AO_Track_Flush");
  LOAD(pause, "HI_MPI_AO_Track_Pause");
  LOAD(start, "HI_MPI_AO_Track_Start");
  LOAD(volume, "HI_UNF_SND_SetTrackAbsWeight");
#undef LOAD
  rc = pcm->init();
  if (rc != 0)
    goto fail;
  pcm->initialized = 1;
  /* The vendor service keeps sound device 0 open. Do not open/close that
   * shared device or change HDMI/SPDIF modes and master-track ownership. */
  rc = pcm->default_attr(1, &attr); /* slave */
  if (rc != 0)
    goto fail;
  rc = pcm->create(0, &attr, &pcm->track);
  if (rc != 0)
  {
    pcm->track = UINT32_MAX;
    goto fail;
  }
  *output = pcm;
  return STBP_OK;
fail:
  pcm_error(pcm, "open", rc);
  stbp_hisi_pcm_close(pcm);
  return STBP_ERROR_IO;
}

void stbp_hisi_pcm_close(struct stbp_hisi_pcm* pcm)
{
  if (pcm == NULL)
    return;
  if (pcm->track != UINT32_MAX)
  {
    const int rc = pcm->destroy(pcm->track);
    if (rc != 0) pcm_error(pcm, "destroy track", rc);
  }
  if (pcm->initialized)
  {
    const int rc = pcm->deinit();
    if (rc != 0) pcm_error(pcm, "deinit", rc);
  }
  free(pcm);
}

enum stbp_result stbp_hisi_pcm_queue(struct stbp_hisi_pcm* pcm,
    const struct stbp_audio_packet* packet)
{
  const size_t frame_bytes = pcm->config.channels * sizeof(int16_t);
  if (packet->size % frame_bytes != 0 || packet->size / frame_bytes > UINT32_MAX)
    return STBP_ERROR_INVALID_ARGUMENT;
  if (pcm->paused)
    return STBP_AGAIN;
  struct hi_pcm_frame frame = {.bits = 16, .interleaved = 1,
    .rate = pcm->config.sample_rate, .channels = pcm->config.channels,
    .pts_ms = (uint32_t)(pcm->frames * 1000 / pcm->config.sample_rate),
    .pcm = (int32_t*)(void*)packet->data, .samples = (uint32_t)(packet->size / frame_bytes),
    .index = pcm->index};
  const int rc = pcm->send(pcm->track, &frame);
  if ((uint32_t)rc == UINT32_C(0x80130046)) /* HI_ERR_AO_OUT_BUF_FULL */
    return STBP_AGAIN;
  if (rc != 0)
    return pcm_error(pcm, "send", rc);
  pcm->frames += frame.samples;
  ++pcm->index;
  return STBP_OK;
}

enum stbp_result stbp_hisi_pcm_pts(struct stbp_hisi_pcm* pcm, int64_t* pts)
{
  uint32_t delay_ms = 0;
  const int rc = pcm->delay(pcm->track, &delay_ms);
  if (rc != 0)
    return pcm_error(pcm, "delay", rc);
  const int64_t submitted = (int64_t)(pcm->frames * 90000 / pcm->config.sample_rate);
  const int64_t played = submitted - (int64_t)delay_ms * 90;
  /* Millisecond delay quantization must not make elapsed samples run backwards.
   * A fresh track/flush starts at zero, independently of the media seek time. */
  if (played > pcm->played)
    pcm->played = played;
  *pts = pcm->played;
  return STBP_OK;
}

enum stbp_result stbp_hisi_pcm_flush(struct stbp_hisi_pcm* pcm)
{
  const int rc = pcm->flush(pcm->track);
  if (rc != 0)
    return pcm_error(pcm, "flush", rc);
  pcm->frames = 0;
  pcm->played = 0;
  pcm->index = 0;
  return STBP_OK;
}

enum stbp_result stbp_hisi_pcm_drain(struct stbp_hisi_pcm* pcm)
{
  const struct timespec interval = {0, 10000000};
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    uint32_t delay_ms = 0;
    const int rc = pcm->delay(pcm->track, &delay_ms);
    if (rc != 0)
      return pcm_error(pcm, "drain", rc);
    if (delay_ms == 0)
      return STBP_OK;
    (void)nanosleep(&interval, NULL);
  }
  return STBP_AGAIN;
}

enum stbp_result stbp_hisi_pcm_pause(struct stbp_hisi_pcm* pcm, int paused)
{
  const int rc = paused ? pcm->pause(pcm->track) : pcm->start(pcm->track);
  if (rc != 0)
    return pcm_error(pcm, "pause/resume", rc);
  pcm->paused = paused != 0;
  return STBP_OK;
}

enum stbp_result stbp_hisi_pcm_volume(struct stbp_hisi_pcm* pcm, uint32_t left, uint32_t right)
{
  const struct hi_pcm_gain gain = {1, (int32_t)left, (int32_t)right};
  const int rc = pcm->volume(pcm->track, &gain);
  return rc == 0 ? STBP_OK : pcm_error(pcm, "track volume", rc);
}
