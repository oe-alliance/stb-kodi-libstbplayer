/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Broadcom Linux-DVB/PES backend for ARM and MIPSel receivers.
 *
 * The ioctl order and stream-type values intentionally follow the
 * gstreamer1.0-plugin-multibox-dvbmediasink path used by Enigma2 on HD51.
 * Kodi still owns demuxing, audio, timing, subtitles and all player control;
 * this module only submits compressed video access units to the hardware decoder.
 */
#include "stbplayer/backend.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dvb/video.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BCM_VIDEO_DEVICE "/dev/dvb/adapter0/video0"
#define BCM_FALLBACK_FRAMERATE "/proc/stb/vmpeg/0/fallback_framerate"
#define BCM_STC_HOST_DEVICE "/dev/stb-stc-host"
#define BCM_NEXUS_STC_SETTINGS_SIZE 512U
#define BCM_NEXUS_STC_MODE_OFFSET sizeof(uint32_t)
#define BCM_NEXUS_STC_MODE_HOST 2U
#ifndef STBP_BCM_DVB_NEXUS_STC
#define STBP_BCM_DVB_NEXUS_STC 0
#endif
#ifndef STBP_BCM_DVB_STARTUP_CATCHUP
#define STBP_BCM_DVB_STARTUP_CATCHUP 0
#endif
#if STBP_BCM_DVB_STARTUP_CATCHUP && \
    (defined(STBP_BCM_DVB_VARIANT_NORMAL) || defined(STBP_BCM_DVB_VARIANT_VUPLUS))
#define BCM_STARTUP_PREROLL_CATCHUP 1
#else
#define BCM_STARTUP_PREROLL_CATCHUP 0
#endif
#if defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
#define BCM_VIDEO_MODE "/proc/stb/video/videomode"
#define BCM_VIDEO_MODE_24HZ "/proc/stb/video/videomode_24hz"
#define BCM_VIDEO_MODE_50HZ "/proc/stb/video/videomode_50hz"
#define BCM_VIDEO_MODE_60HZ "/proc/stb/video/videomode_60hz"
#define BCM_VIDEO_MODE_SIZE 64U
#endif
#define BCM_STREAMTYPE_MPEG2 0
#define BCM_STREAMTYPE_H264 1
#define BCM_STREAMTYPE_H263 2
#define BCM_STREAMTYPE_MPEG4_PART2 4
#define BCM_STREAMTYPE_MPEG1 6
#define BCM_STREAMTYPE_XVID 10
#define BCM_STREAMTYPE_DIVX3 13
#define BCM_STREAMTYPE_DIVX4 14
#define BCM_STREAMTYPE_DIVX5 15
#define BCM_STREAMTYPE_VP6 18
#define BCM_STREAMTYPE_SPARK 21

#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
#define BCM_STREAMTYPE_HEVC 22
#define BCM_STREAMTYPE_VC1 16
#define BCM_STREAMTYPE_WMV3 17
#define BCM_STREAMTYPE_VP8 20
#define BCM_STREAMTYPE_VP9 23
#elif defined(STBP_BCM_DVB_VARIANT_TYPE2)
#define BCM_STREAMTYPE_HEVC 7
#define BCM_STREAMTYPE_VC1 3
#define BCM_STREAMTYPE_WMV3 5
#define BCM_STREAMTYPE_VP8 20
#define BCM_STREAMTYPE_VP9 23
#else
#define BCM_STREAMTYPE_HEVC 7
#define BCM_STREAMTYPE_VC1 3
#define BCM_STREAMTYPE_WMV3 5
#define BCM_STREAMTYPE_VP8 8
#define BCM_STREAMTYPE_VP9 9
#endif

#define BCM_MAX_PACKET_SIZE (16U * 1024U * 1024U)
#define BCM_PREFERRED_QUEUE_BYTES (4U * 1024U * 1024U)
#define BCM_VP9_CHUNK_SIZE 0x8000U
#define BCM_VP9_FIRST_PES_SIZE 0x8008U
#define BCM_VP9_TRAILER_SIZE 184U

typedef void (*bcm_nexus_stc_get_default_fn)(unsigned int, void*);
typedef void* (*bcm_nexus_stc_open_fn)(unsigned int, const void*);
typedef void (*bcm_nexus_stc_close_fn)(void*);
typedef int (*bcm_nexus_stc_set_stc_fn)(void*, uint32_t);
typedef int (*bcm_nexus_stc_freeze_fn)(void*, int);
typedef int (*bcm_nexus_stc_set_rate_fn)(void*, unsigned int, unsigned int);

struct bcm_instance
{
  struct stbp_host_callbacks host;
  pthread_mutex_t mutex;
  enum stbp_state state;
  enum stbp_result last_error;
  enum stbp_codec codec;
  int video_fd;
  uint8_t* codec_data;
  size_t codec_data_size;
  int send_codec_data;
  uint8_t* pending;
  size_t pending_size;
  size_t pending_offset;
  size_t* pending_boundaries;
  size_t pending_boundary_count;
  size_t pending_boundary_index;
  uint64_t packets_queued;
  uint64_t packets_dropped;
  int64_t last_pts_90k;
  int stc_host_enabled;
  int stc_needs_seed;
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
  int decoder_has_data;
  int resume_startup_pending;
#endif
  int startup_preroll;
  uint32_t startup_preroll_packets;
  int64_t startup_preroll_first_pts_90k;
  int64_t startup_preroll_last_pts_90k;
  int64_t startup_sync_floor_pts_90k;
#if BCM_STARTUP_PREROLL_CATCHUP
  int startup_catchup_active;
  int64_t startup_catchup_target_pts_90k;
  uint64_t startup_catchup_deadline_ms;
#endif
#if STBP_BCM_DVB_NEXUS_STC
  void* nexus_library;
  void* nexus_stc_channel;
  bcm_nexus_stc_close_fn nexus_stc_close;
  bcm_nexus_stc_set_stc_fn nexus_stc_set_stc;
  bcm_nexus_stc_freeze_fn nexus_stc_freeze;
  bcm_nexus_stc_set_rate_fn nexus_stc_set_rate;
#endif
#if defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
  int video_modes_pinned;
  char video_mode_24hz[BCM_VIDEO_MODE_SIZE];
  char video_mode_50hz[BCM_VIDEO_MODE_SIZE];
  char video_mode_60hz[BCM_VIDEO_MODE_SIZE];
#endif
};

static void bcm_log(struct bcm_instance* instance,
                    enum stbp_log_level level,
                    const char* message)
{
  if (instance != NULL && instance->host.log != NULL)
    instance->host.log(instance->host.userdata, level, message);
}

static void bcm_log_errno(struct bcm_instance* instance,
                          enum stbp_log_level level,
                          const char* operation)
{
  char message[256];
  const int saved_errno = errno;
  (void)snprintf(message, sizeof(message), "%s failed: %s (%d)", operation,
                 strerror(saved_errno), saved_errno);
  bcm_log(instance, level, message);
}

static int write_stc_command(struct bcm_instance* instance, const char* command)
{
  const size_t size = strlen(command);
  int fd;
  ssize_t written;

  fd = open(BCM_STC_HOST_DEVICE, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  do
  {
    written = write(fd, command, size);
  } while (written < 0 && errno == EINTR);
  (void)close(fd);
  if (written != (ssize_t)size)
  {
    bcm_log_errno(instance, STBP_LOG_WARNING, "setting Broadcom host STC");
    return 0;
  }
  return 1;
}

#if STBP_BCM_DVB_NEXUS_STC
static int open_nexus_host_stc(struct bcm_instance* instance)
{
  unsigned char settings[BCM_NEXUS_STC_SETTINGS_SIZE] __attribute__((aligned(8)));
  const uint32_t host_mode = BCM_NEXUS_STC_MODE_HOST;
  bcm_nexus_stc_get_default_fn get_default;
  bcm_nexus_stc_open_fn open_channel;

  if (instance->nexus_stc_channel != NULL)
    return 1;
  instance->nexus_library = dlopen("libnexus.so", RTLD_NOW | RTLD_LOCAL);
  if (instance->nexus_library == NULL)
    return 0;
  get_default = (bcm_nexus_stc_get_default_fn)dlsym(
      instance->nexus_library, "NEXUS_StcChannel_GetDefaultSettings");
  open_channel = (bcm_nexus_stc_open_fn)dlsym(
      instance->nexus_library, "NEXUS_StcChannel_Open");
  instance->nexus_stc_close = (bcm_nexus_stc_close_fn)dlsym(
      instance->nexus_library, "NEXUS_StcChannel_Close");
  instance->nexus_stc_set_stc = (bcm_nexus_stc_set_stc_fn)dlsym(
      instance->nexus_library, "NEXUS_StcChannel_SetStc");
  instance->nexus_stc_freeze = (bcm_nexus_stc_freeze_fn)dlsym(
      instance->nexus_library, "NEXUS_StcChannel_Freeze");
  instance->nexus_stc_set_rate = (bcm_nexus_stc_set_rate_fn)dlsym(
      instance->nexus_library, "NEXUS_StcChannel_SetRate");
  if (get_default == NULL || open_channel == NULL ||
      instance->nexus_stc_close == NULL || instance->nexus_stc_set_stc == NULL ||
      instance->nexus_stc_freeze == NULL || instance->nexus_stc_set_rate == NULL)
    goto failed;

  /* The Nexus ABI starts NEXUS_StcChannelSettings with two 32-bit enums:
   * timebase followed by mode.  Keep the vendor-extended tail opaque so this
   * remains compatible with the receiver's installed libnexus revision. */
  memset(settings, 0, sizeof(settings));
  get_default(0, settings);
  memcpy(settings + BCM_NEXUS_STC_MODE_OFFSET, &host_mode, sizeof(host_mode));
  instance->nexus_stc_channel = open_channel(0, settings);
  if (instance->nexus_stc_channel == NULL)
    goto failed;
  bcm_log(instance, STBP_LOG_INFO, "Broadcom Nexus host STC channel opened");
  return 1;

failed:
  instance->nexus_stc_close = NULL;
  instance->nexus_stc_set_stc = NULL;
  instance->nexus_stc_freeze = NULL;
  instance->nexus_stc_set_rate = NULL;
  (void)dlclose(instance->nexus_library);
  instance->nexus_library = NULL;
  return 0;
}

static void close_nexus_host_stc(struct bcm_instance* instance)
{
  if (instance->nexus_stc_channel != NULL && instance->nexus_stc_close != NULL)
    instance->nexus_stc_close(instance->nexus_stc_channel);
  instance->nexus_stc_channel = NULL;
  instance->nexus_stc_close = NULL;
  instance->nexus_stc_set_stc = NULL;
  instance->nexus_stc_freeze = NULL;
  instance->nexus_stc_set_rate = NULL;
  if (instance->nexus_library != NULL)
    (void)dlclose(instance->nexus_library);
  instance->nexus_library = NULL;
}
#endif

static int host_stc_available(const struct bcm_instance* instance)
{
  if (access(BCM_STC_HOST_DEVICE, W_OK) == 0)
    return 1;
#if STBP_BCM_DVB_NEXUS_STC
  return instance->nexus_stc_channel != NULL;
#else
  (void)instance;
  return 0;
#endif
}

#if defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
static int read_proc_value(const char* path, char* value, size_t capacity)
{
  ssize_t length;
  int fd;

  if (value == NULL || capacity < 2U)
    return 0;
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  do
  {
    length = read(fd, value, capacity - 1U);
  } while (length < 0 && errno == EINTR);
  (void)close(fd);
  if (length <= 0)
    return 0;
  while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r' ||
                        value[length - 1] == ' ' || value[length - 1] == '\t'))
    --length;
  value[length] = '\0';
  return length > 0;
}

static int write_proc_value(const char* path, const char* value)
{
  const size_t length = strlen(value);
  ssize_t written;
  int fd;

  fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  do
  {
    written = write(fd, value, length);
  } while (written < 0 && errno == EINTR);
  (void)close(fd);
  return written == (ssize_t)length;
}

static void restore_gigablue_video_modes(struct bcm_instance* instance)
{
  if (!instance->video_modes_pinned)
    return;
  (void)write_proc_value(BCM_VIDEO_MODE_24HZ, instance->video_mode_24hz);
  (void)write_proc_value(BCM_VIDEO_MODE_50HZ, instance->video_mode_50hz);
  (void)write_proc_value(BCM_VIDEO_MODE_60HZ, instance->video_mode_60hz);
  instance->video_modes_pinned = 0;
  bcm_log(instance, STBP_LOG_INFO, "GigaBlue automatic HDMI modes restored");
}

static int pin_gigablue_video_modes(struct bcm_instance* instance)
{
  char current_mode[BCM_VIDEO_MODE_SIZE];

  if (!read_proc_value(BCM_VIDEO_MODE, current_mode, sizeof(current_mode)) ||
      !read_proc_value(BCM_VIDEO_MODE_24HZ, instance->video_mode_24hz,
                       sizeof(instance->video_mode_24hz)) ||
      !read_proc_value(BCM_VIDEO_MODE_50HZ, instance->video_mode_50hz,
                       sizeof(instance->video_mode_50hz)) ||
      !read_proc_value(BCM_VIDEO_MODE_60HZ, instance->video_mode_60hz,
                       sizeof(instance->video_mode_60hz)))
    return 0;

  instance->video_modes_pinned = 1;
  if (!write_proc_value(BCM_VIDEO_MODE_24HZ, current_mode) ||
      !write_proc_value(BCM_VIDEO_MODE_50HZ, current_mode) ||
      !write_proc_value(BCM_VIDEO_MODE_60HZ, current_mode))
  {
    restore_gigablue_video_modes(instance);
    return 0;
  }
  bcm_log(instance, STBP_LOG_INFO, "GigaBlue automatic HDMI modes pinned to Kodi output");
  return 1;
}
#endif

static int seed_host_stc(struct bcm_instance* instance, int64_t pts_90k)
{
  char command[48];
  char message[160];
  uint32_t stc;
  int length;

  if (pts_90k == STBP_PTS_NONE)
    return 0;
  /* Kodi's initial GENERAL_RESYNC can legitimately carry a small negative
   * preroll timestamp. The hardware clock is unsigned; wrapping that value
   * near UINT32_MAX makes the decoder wait instead of presenting PTS zero. */
  stc = pts_90k < 0 ? 0U : (uint32_t)pts_90k;
#if STBP_BCM_DVB_NEXUS_STC
  if (instance->nexus_stc_channel != NULL && instance->nexus_stc_set_stc != NULL)
  {
    if (instance->nexus_stc_set_rate == NULL || instance->nexus_stc_freeze == NULL ||
        /* This Nexus generation advances the host clock at 45 kHz for rate
         * (1,0). MPEG/Kodi PTS use 90 kHz; the measured normal rate is (2,0). */
        instance->nexus_stc_set_rate(instance->nexus_stc_channel, 2U, 0U) != 0 ||
        instance->nexus_stc_set_stc(instance->nexus_stc_channel,
                                    stc) != 0 ||
        instance->nexus_stc_freeze(instance->nexus_stc_channel, 0) != 0)
    {
      instance->stc_needs_seed = 0;
      return 0;
    }
    instance->stc_host_enabled = 1;
    instance->stc_needs_seed = 0;
    (void)snprintf(message, sizeof(message),
                   "Broadcom Nexus clock switched to host STC: clock=%u, request=%lld, queued-video=%lld",
                   stc, (long long)pts_90k, (long long)instance->last_pts_90k);
    bcm_log(instance, STBP_LOG_INFO, message);
    return 1;
  }
#endif
  length = snprintf(command, sizeof(command), "host %u", stc);
  if (length <= 0 || (size_t)length >= sizeof(command) ||
      !write_stc_command(instance, command))
  {
    instance->stc_needs_seed = 0;
    return 0;
  }
  instance->stc_host_enabled = 1;
  instance->stc_needs_seed = 0;
  bcm_log(instance, STBP_LOG_INFO, "Broadcom video clock switched to host STC");
  return 1;
}

static int codec_to_stream_type(enum stbp_codec codec)
{
  switch (codec)
  {
    case STBP_CODEC_MPEG1:
      return BCM_STREAMTYPE_MPEG1;
    case STBP_CODEC_MPEG2:
      return BCM_STREAMTYPE_MPEG2;
    case STBP_CODEC_MPEG4_PART2:
      return BCM_STREAMTYPE_MPEG4_PART2;
    case STBP_CODEC_MSMPEG4V3:
      return BCM_STREAMTYPE_DIVX3;
    case STBP_CODEC_DIVX4:
      return BCM_STREAMTYPE_DIVX4;
    case STBP_CODEC_DIVX5:
      return BCM_STREAMTYPE_DIVX5;
    case STBP_CODEC_XVID:
      return BCM_STREAMTYPE_XVID;
    case STBP_CODEC_H263:
      return BCM_STREAMTYPE_H263;
    case STBP_CODEC_H264:
      return BCM_STREAMTYPE_H264;
#if STBP_BCM_DVB_HAVE_HEVC
    case STBP_CODEC_HEVC:
      return BCM_STREAMTYPE_HEVC;
#endif
#if STBP_BCM_DVB_HAVE_WMV
    case STBP_CODEC_VC1:
      return BCM_STREAMTYPE_VC1;
    case STBP_CODEC_WMV3:
      return BCM_STREAMTYPE_WMV3;
#endif
#if STBP_BCM_DVB_HAVE_VP6
    case STBP_CODEC_VP6:
      return BCM_STREAMTYPE_VP6;
#endif
#if STBP_BCM_DVB_HAVE_VP8
    case STBP_CODEC_VP8:
      return BCM_STREAMTYPE_VP8;
#endif
#if STBP_BCM_DVB_HAVE_VP9
    case STBP_CODEC_VP9:
      return BCM_STREAMTYPE_VP9;
#endif
#if STBP_BCM_DVB_HAVE_SPARK
    case STBP_CODEC_FLV1:
      return BCM_STREAMTYPE_SPARK;
#endif
    default:
      return -1;
  }
}

static int codec_uses_annex_b(enum stbp_codec codec)
{
  return codec == STBP_CODEC_H264 || codec == STBP_CODEC_HEVC;
}

static int codec_uses_bcmv(enum stbp_codec codec)
{
  return codec == STBP_CODEC_VP6 || codec == STBP_CODEC_VP8 ||
         codec == STBP_CODEC_VP9 || codec == STBP_CODEC_FLV1;
}

static int has_start_code(const uint8_t* data, size_t size)
{
  return data != NULL && size >= 3U && data[0] == 0U && data[1] == 0U && data[2] == 1U;
}

static size_t make_pes_header(uint8_t* header, int64_t pts_90k);
static void set_pes_payload_size(uint8_t* header, size_t payload_size);

static void release_pending(struct bcm_instance* instance)
{
  free(instance->pending);
  free(instance->pending_boundaries);
  instance->pending = NULL;
  instance->pending_size = 0;
  instance->pending_offset = 0;
  instance->pending_boundaries = NULL;
  instance->pending_boundary_count = 0;
  instance->pending_boundary_index = 0;
}

static void release_codec_data(struct bcm_instance* instance)
{
  free(instance->codec_data);
  instance->codec_data = NULL;
  instance->codec_data_size = 0;
  instance->send_codec_data = 0;
}

static int copy_codec_data(struct bcm_instance* instance, const uint8_t* data, size_t size)
{
  if (size == 0U)
    return 1;
  instance->codec_data = (uint8_t*)malloc(size);
  if (instance->codec_data == NULL)
    return 0;
  memcpy(instance->codec_data, data, size);
  instance->codec_data_size = size;
  instance->send_codec_data = 1;
  return 1;
}

static int configure_codec_data(struct bcm_instance* instance,
                                const struct stbp_stream_config* stream)
{
  static const uint8_t divx4_header[] = {
      0x00, 0x00, 0x01, 0xb2, 'D', 'i', 'v', 'X', '4', 'A', 'N', 'D'};
  static const uint8_t divx3_header_template[63] = {
      0x00, 0x00, 0x01, 0xe0, 0x00, 0x34, 0x80, 0x80,
      0x05, 0x2f, 0xff, 0xff, 0xff, 0xff,
      0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x20,
      0x08, 0xc8, 0x0d, 0x40, 0x00, 0x53, 0x88, 0x40,
      0x0c, 0x40, 0x01, 0x90, 0x00, 0x97, 0x53, 0x0a,
      0x00, 0x00, 0x00, 0x00,
      0x30, 0x7f, 0x00, 0x00, 0x01, 0xb2, 0x44, 0x69,
      0x76, 0x58, 0x33, 0x31, 0x31, 0x41, 0x4e, 0x44,
      0x00};

  if (codec_uses_bcmv(stream->codec))
    return 1;
  if (stream->codec == STBP_CODEC_MSMPEG4V3)
  {
    uint8_t header[sizeof(divx3_header_template)];
    const unsigned int width = stream->width;
    const unsigned int height = stream->height;
    memcpy(header, divx3_header_template, sizeof(header));
    header[38] = (uint8_t)((width >> 4) & 0xffU);
    header[39] = (uint8_t)(((width & 0x0fU) << 4) | 0x08U | ((height >> 10) & 0x03U));
    header[40] = (uint8_t)((height >> 2) & 0xffU);
    header[41] = (uint8_t)(((height & 0x03U) << 6) | 0x20U);
    return copy_codec_data(instance, header, sizeof(header));
  }
  if (stream->codec == STBP_CODEC_DIVX4)
    return copy_codec_data(instance, divx4_header, sizeof(divx4_header));
  return copy_codec_data(instance, stream->extra_data, stream->extra_data_size);
}

static size_t packet_prefix(enum stbp_codec codec,
                            const uint8_t* data,
                            size_t size,
                            uint8_t prefix[4])
{
  if ((codec == STBP_CODEC_MPEG4_PART2 || codec == STBP_CODEC_XVID) &&
      !has_start_code(data, size))
  {
    prefix[0] = 0x00;
    prefix[1] = 0x00;
    prefix[2] = 0x01;
    return 3U;
  }
  if ((codec == STBP_CODEC_MSMPEG4V3 || codec == STBP_CODEC_DIVX4) &&
      (size < 4U || memcmp(data, "\x00\x00\x01\xb6", 4U) != 0))
  {
    prefix[0] = 0x00;
    prefix[1] = 0x00;
    prefix[2] = 0x01;
    prefix[3] = 0xb6;
    return 4U;
  }
  if ((codec == STBP_CODEC_VC1 || codec == STBP_CODEC_WMV3) &&
      !has_start_code(data, size))
  {
    prefix[0] = 0x00;
    prefix[1] = 0x00;
    prefix[2] = 0x01;
    prefix[3] = 0x0d;
    return 4U;
  }
  return 0U;
}

static enum stbp_result prepare_standard_packet_locked(struct bcm_instance* instance,
                                                       const struct stbp_packet* packet,
                                                       int64_t pts)
{
  uint8_t pes_header[14];
  uint8_t prefix[4];
  const size_t pes_header_size = make_pes_header(pes_header, pts);
  const size_t prefix_size = packet_prefix(instance->codec, packet->data, packet->size,
                                           prefix);
  const int raw_codec_data = instance->send_codec_data &&
                             instance->codec == STBP_CODEC_MSMPEG4V3;
  const size_t raw_codec_size = raw_codec_data ? instance->codec_data_size : 0U;
  const size_t pes_codec_size = instance->send_codec_data && !raw_codec_data
                                    ? instance->codec_data_size
                                    : 0U;
  size_t payload_size;
  size_t total_size;
  size_t offset = 0U;

  if ((instance->codec == STBP_CODEC_VC1 || instance->codec == STBP_CODEC_WMV3) &&
      (packet->flags & STBP_PACKET_KEYFRAME) != 0)
    pes_header[6] = 0x80;
  if (pes_header_size - 6U > SIZE_MAX - pes_codec_size ||
      pes_header_size - 6U + pes_codec_size > SIZE_MAX - prefix_size ||
      pes_header_size - 6U + pes_codec_size + prefix_size > SIZE_MAX - packet->size)
    return STBP_ERROR_INVALID_ARGUMENT;
  payload_size = pes_header_size - 6U + pes_codec_size + prefix_size + packet->size;
  if (raw_codec_size > SIZE_MAX - pes_header_size ||
      raw_codec_size + pes_header_size > SIZE_MAX - pes_codec_size ||
      raw_codec_size + pes_header_size + pes_codec_size > SIZE_MAX - prefix_size ||
      raw_codec_size + pes_header_size + pes_codec_size + prefix_size >
          SIZE_MAX - packet->size)
    return STBP_ERROR_INVALID_ARGUMENT;
  total_size = raw_codec_size + pes_header_size + pes_codec_size + prefix_size + packet->size;
  set_pes_payload_size(pes_header, payload_size);

  instance->pending = (uint8_t*)malloc(total_size);
  if (instance->pending == NULL)
    return STBP_ERROR_BACKEND;
#if defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
  /* Gigablue's reference dvbvideosink submits codec data, the PES header and
   * the encoded access unit in separate writes.  Its Nexus DVB driver parses
   * the stream headers from a combined write but never presents a frame
   * (VIDEO_GET_PTS remains zero).  Preserve the reference syscall boundaries. */
  instance->pending_boundaries =
      (size_t*)malloc((raw_codec_size != 0U ? 2U : 1U) *
                      sizeof(*instance->pending_boundaries));
  if (instance->pending_boundaries == NULL)
  {
    release_pending(instance);
    return STBP_ERROR_BACKEND;
  }
#endif
  if (raw_codec_size != 0U)
  {
    memcpy(instance->pending + offset, instance->codec_data, raw_codec_size);
    offset += raw_codec_size;
#if defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
    instance->pending_boundaries[instance->pending_boundary_count++] = offset;
#endif
  }
  memcpy(instance->pending + offset, pes_header, pes_header_size);
  offset += pes_header_size;
  if (pes_codec_size != 0U)
  {
    memcpy(instance->pending + offset, instance->codec_data, pes_codec_size);
    offset += pes_codec_size;
  }
  if (prefix_size != 0U)
  {
    memcpy(instance->pending + offset, prefix, prefix_size);
    offset += prefix_size;
  }
#if defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
  instance->pending_boundaries[instance->pending_boundary_count++] = offset;
  instance->pending_boundary_index = 0U;
#endif
  if (packet->size != 0U)
    memcpy(instance->pending + offset, packet->data, packet->size);
  instance->pending_size = total_size;
  instance->pending_offset = 0U;
  instance->send_codec_data = 0;
  return STBP_OK;
}

static size_t make_bcmv_header(uint8_t* header, enum stbp_codec codec, size_t data_size)
{
  const uint32_t length = (uint32_t)(data_size + 10U +
                                     (codec == STBP_CODEC_VP6 ? 1U : 0U));
  size_t offset = 0U;
  memcpy(header + offset, "BCMV", 4U);
  offset += 4U;
  header[offset++] = (uint8_t)(length >> 24);
  header[offset++] = (uint8_t)(length >> 16);
  header[offset++] = (uint8_t)(length >> 8);
  header[offset++] = (uint8_t)length;
  header[offset++] = 0U;
#if defined(STBP_BCM_DVB_VARIANT_VUPLUS)
  header[offset++] = 0U;
#else
  header[offset++] = codec == STBP_CODEC_VP9 ? 1U : 0U;
#endif
  if (codec == STBP_CODEC_VP6)
    header[offset++] = 0U;
  return offset;
}

static void make_vp9_trailer(uint8_t trailer[BCM_VP9_TRAILER_SIZE])
{
  memset(trailer, 0, BCM_VP9_TRAILER_SIZE);
  trailer[2] = 0x01;
  trailer[3] = 0xe0;
  trailer[4] = 0x00;
  trailer[5] = 0xb2;
  trailer[6] = 0x81;
  trailer[7] = 0x01;
  trailer[8] = 0x14;
  trailer[9] = 0x80;
  memcpy(trailer + 10U, "BRCM", 4U);
  trailer[26] = 0xff;
  trailer[27] = 0xff;
  trailer[28] = 0xff;
  trailer[29] = 0xff;
  trailer[33] = 0x85;
}

static enum stbp_result prepare_bcmv_packet_locked(struct bcm_instance* instance,
                                                   const struct stbp_packet* packet,
                                                   int64_t pts)
{
  uint8_t pes_header[14];
  uint8_t bcmv_header[11];
  const size_t pes_header_size = make_pes_header(pes_header, pts);
  const size_t bcmv_header_size = make_bcmv_header(bcmv_header, instance->codec,
                                                   packet->size);
  size_t total_size;
  size_t offset = 0U;

#if defined(STBP_BCM_DVB_VARIANT_VUPLUS)
  /* Vu+ dvbvideosink replaces the first four encoded PTS bytes for VP9 with
   * a native-endian 45 kHz value.  The proprietary VU decoder expects this
   * quirk even though the surrounding PES header otherwise carries 90 kHz. */
  if (instance->codec == STBP_CODEC_VP9 && pts != STBP_PTS_NONE && pts >= 0)
  {
    const uint32_t vu_vp9_pts = (uint32_t)((uint64_t)pts / 2U);
    memcpy(pes_header + 9U, &vu_vp9_pts, sizeof(vu_vp9_pts));
  }
#endif

#if !defined(STBP_BCM_DVB_VARIANT_VUPLUS)
  if (instance->codec == STBP_CODEC_VP9)
  {
    uint8_t continuation_header[14];
    uint8_t trailer[BCM_VP9_TRAILER_SIZE];
    const size_t first_overhead = pes_header_size - 6U + bcmv_header_size;
    const size_t first_data = packet->size < BCM_VP9_FIRST_PES_SIZE - first_overhead
                                  ? packet->size
                                  : BCM_VP9_FIRST_PES_SIZE - first_overhead;
    const size_t remaining = packet->size - first_data;
    const size_t continuation_count =
        remaining == 0U ? 0U : (remaining + BCM_VP9_CHUNK_SIZE - 1U) / BCM_VP9_CHUNK_SIZE;
    const size_t boundary_count = 3U + continuation_count * 2U;
    size_t boundary_index = 0U;
    size_t data_offset = first_data;
    size_t left = remaining;

    if (continuation_count > (SIZE_MAX - pes_header_size - bcmv_header_size -
                              packet->size - BCM_VP9_TRAILER_SIZE) / 9U)
      return STBP_ERROR_INVALID_ARGUMENT;
    total_size = pes_header_size + bcmv_header_size + packet->size +
                 continuation_count * 9U + BCM_VP9_TRAILER_SIZE;
    instance->pending = (uint8_t*)malloc(total_size);
    if (instance->pending == NULL)
      return STBP_ERROR_BACKEND;
    instance->pending_boundaries =
        (size_t*)malloc(boundary_count * sizeof(*instance->pending_boundaries));
    if (instance->pending_boundaries == NULL)
    {
      release_pending(instance);
      return STBP_ERROR_BACKEND;
    }

    set_pes_payload_size(pes_header, first_overhead + first_data);
    memcpy(instance->pending + offset, pes_header, pes_header_size);
    offset += pes_header_size;
    memcpy(instance->pending + offset, bcmv_header, bcmv_header_size);
    offset += bcmv_header_size;
    instance->pending_boundaries[boundary_index++] = offset;
    if (first_data != 0U)
    {
      memcpy(instance->pending + offset, packet->data, first_data);
      offset += first_data;
    }
    instance->pending_boundaries[boundary_index++] = offset;
    while (left != 0U)
    {
      const size_t chunk = left < BCM_VP9_CHUNK_SIZE ? left : BCM_VP9_CHUNK_SIZE;
      const size_t continuation_size = make_pes_header(continuation_header, STBP_PTS_NONE);
      set_pes_payload_size(continuation_header, chunk + 3U);
      memcpy(instance->pending + offset, continuation_header, continuation_size);
      offset += continuation_size;
      instance->pending_boundaries[boundary_index++] = offset;
      memcpy(instance->pending + offset, packet->data + data_offset, chunk);
      offset += chunk;
      instance->pending_boundaries[boundary_index++] = offset;
      data_offset += chunk;
      left -= chunk;
    }
    make_vp9_trailer(trailer);
    memcpy(instance->pending + offset, trailer, sizeof(trailer));
    offset += sizeof(trailer);
    instance->pending_boundaries[boundary_index++] = offset;
    instance->pending_size = offset;
    instance->pending_offset = 0U;
    instance->pending_boundary_count = boundary_index;
    instance->pending_boundary_index = 0U;
    return STBP_OK;
  }
#endif

  if (pes_header_size > SIZE_MAX - bcmv_header_size ||
      pes_header_size + bcmv_header_size > SIZE_MAX - packet->size)
    return STBP_ERROR_INVALID_ARGUMENT;
  total_size = pes_header_size + bcmv_header_size + packet->size;
  set_pes_payload_size(pes_header, pes_header_size - 6U + bcmv_header_size + packet->size);
  instance->pending = (uint8_t*)malloc(total_size);
  if (instance->pending == NULL)
    return STBP_ERROR_BACKEND;
#if defined(STBP_BCM_DVB_VARIANT_VUPLUS)
  /* VU's reference dvbvideosink writes PES/BCMV and frame data in separate
   * syscalls.  Preserve that boundary; combining both keeps VP9 on the stale
   * Enigma2 frame even though VIDEO_PLAY succeeds. */
  instance->pending_boundaries =
      (size_t*)malloc(sizeof(*instance->pending_boundaries));
  if (instance->pending_boundaries == NULL)
  {
    release_pending(instance);
    return STBP_ERROR_BACKEND;
  }
#endif
  memcpy(instance->pending + offset, pes_header, pes_header_size);
  offset += pes_header_size;
  memcpy(instance->pending + offset, bcmv_header, bcmv_header_size);
  offset += bcmv_header_size;
#if defined(STBP_BCM_DVB_VARIANT_VUPLUS)
  instance->pending_boundaries[0] = offset;
  instance->pending_boundary_count = 1U;
  instance->pending_boundary_index = 0U;
#endif
  if (packet->size != 0U)
    memcpy(instance->pending + offset, packet->data, packet->size);
  instance->pending_size = total_size;
  instance->pending_offset = 0U;
  return STBP_OK;
}

static enum stbp_result flush_pending_locked(struct bcm_instance* instance)
{
  while (instance->pending_offset < instance->pending_size)
  {
    size_t write_end = instance->pending_size;
    while (instance->pending_boundary_index < instance->pending_boundary_count &&
           instance->pending_offset >=
               instance->pending_boundaries[instance->pending_boundary_index])
      ++instance->pending_boundary_index;
    if (instance->pending_boundary_index < instance->pending_boundary_count)
      write_end = instance->pending_boundaries[instance->pending_boundary_index];

    /* The Broadcom VP9 path requires the same syscall boundaries as
     * dvbvideosink: PES/BCMV header, payload chunks, and trailer are written
     * separately. Combining adjacent segments makes BCM7251S stop decoding. */
    const ssize_t written = write(instance->video_fd,
                                  instance->pending + instance->pending_offset,
                                  write_end - instance->pending_offset);
    if (written > 0)
    {
      instance->pending_offset += (size_t)written;
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return STBP_AGAIN;
    bcm_log_errno(instance, STBP_LOG_ERROR, "writing DVB video PES");
    instance->last_error = STBP_ERROR_IO;
    return STBP_ERROR_IO;
  }

  release_pending(instance);
  return STBP_OK;
}

static size_t make_pes_header(uint8_t* header, int64_t pts_90k)
{
  uint64_t pts;

  header[0] = 0x00;
  header[1] = 0x00;
  header[2] = 0x01;
  header[3] = 0xe0;
  /* An unbounded video PES packet is accepted by the Broadcom DVB driver. */
  header[4] = 0x00;
  header[5] = 0x00;
  header[6] = 0x81;

  if (pts_90k == STBP_PTS_NONE || pts_90k < 0)
  {
    header[7] = 0x00;
    header[8] = 0x00;
    return 9;
  }

  pts = (uint64_t)pts_90k & UINT64_C(0x1ffffffff);
  header[7] = 0x80;
  header[8] = 0x05;
  header[9] = (uint8_t)(0x21U | ((pts >> 29) & 0x0eU));
  header[10] = (uint8_t)(pts >> 22);
  header[11] = (uint8_t)(0x01U | ((pts >> 14) & 0xfeU));
  header[12] = (uint8_t)(pts >> 7);
  header[13] = (uint8_t)(0x01U | ((pts << 1) & 0xfeU));
  return 14;
}

static void set_pes_payload_size(uint8_t* header, size_t payload_size)
{
  /* A zero PES_packet_length means unbounded video payload. This is the same
   * fallback used by dvbvideosink when a video access unit exceeds 64 KiB. */
  if (payload_size > UINT16_MAX)
    payload_size = 0;
  header[4] = (uint8_t)(payload_size >> 8);
  header[5] = (uint8_t)payload_size;
}

static void set_fallback_framerate(const struct stbp_rational* frame_rate)
{
  static const unsigned int valid_rates[] = {
      23976U, 24000U, 25000U, 29970U, 30000U, 50000U, 59940U, 60000U};
  uint64_t requested;
  unsigned int best;
  uint64_t best_difference;
  char value[16];
  int fd;
  int length;
  size_t index;

  if (frame_rate == NULL || frame_rate->numerator <= 0 || frame_rate->denominator <= 0)
    return;
  requested = (uint64_t)frame_rate->numerator * UINT64_C(1000) /
              (uint64_t)frame_rate->denominator;
  best = valid_rates[0];
  best_difference = requested > best ? requested - best : best - requested;
  for (index = 1; index < sizeof(valid_rates) / sizeof(valid_rates[0]); ++index)
  {
    const uint64_t difference = requested > valid_rates[index]
                                    ? requested - valid_rates[index]
                                    : valid_rates[index] - requested;
    if (difference < best_difference)
    {
      best = valid_rates[index];
      best_difference = difference;
    }
  }

  length = snprintf(value, sizeof(value), "%u", best);
  if (length <= 0 || (size_t)length >= sizeof(value))
    return;
  fd = open(BCM_FALLBACK_FRAMERATE, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return;
  (void)write(fd, value, (size_t)length);
  (void)close(fd);
}

static int fd_can_accept_packet(int fd)
{
  struct pollfd descriptor;
  int result;

  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.fd = fd;
  descriptor.events = POLLOUT;
  do
  {
    result = poll(&descriptor, 1, 0);
  } while (result < 0 && errno == EINTR);
  return result > 0 && (descriptor.revents & POLLOUT) != 0;
}

#if BCM_STARTUP_PREROLL_CATCHUP
static uint64_t monotonic_milliseconds(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void stop_startup_catchup_locked(struct bcm_instance* instance, const char* reason)
{
  if (!instance->startup_catchup_active)
    return;
  (void)ioctl(instance->video_fd, VIDEO_FAST_FORWARD, 0);
  (void)ioctl(instance->video_fd, VIDEO_CONTINUE);
  instance->startup_catchup_active = 0;
  instance->startup_catchup_target_pts_90k = STBP_PTS_NONE;
  instance->startup_catchup_deadline_ms = 0;
  bcm_log(instance, STBP_LOG_INFO, reason);
}
#endif

static enum stbp_result close_locked(struct bcm_instance* instance)
{
  enum stbp_result final_result = STBP_OK;

  release_pending(instance);
  if (instance->video_fd >= 0)
  {
    if (instance->stc_host_enabled && access(BCM_STC_HOST_DEVICE, W_OK) == 0)
      (void)write_stc_command(instance, "auto");
    instance->stc_host_enabled = 0;
    instance->stc_needs_seed = 0;
    if (ioctl(instance->video_fd, VIDEO_STOP) < 0 && errno != EINVAL)
    {
      bcm_log_errno(instance, STBP_LOG_WARNING, "VIDEO_STOP");
      final_result = STBP_ERROR_IO;
    }
    (void)ioctl(instance->video_fd, VIDEO_SLOWMOTION, 0);
    (void)ioctl(instance->video_fd, VIDEO_FAST_FORWARD, 0);
    (void)ioctl(instance->video_fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX);
    (void)ioctl(instance->video_fd, VIDEO_CLEAR_BUFFER);
    (void)close(instance->video_fd);
    instance->video_fd = -1;
  }
#if STBP_BCM_DVB_NEXUS_STC
  close_nexus_host_stc(instance);
#endif
#if defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
  restore_gigablue_video_modes(instance);
#endif

  release_codec_data(instance);
  instance->state = STBP_STATE_CLOSED;
  instance->codec = STBP_CODEC_UNKNOWN;
  instance->last_pts_90k = STBP_PTS_NONE;
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
  instance->decoder_has_data = 0;
  instance->resume_startup_pending = 0;
#endif
  instance->startup_preroll = 0;
  instance->startup_preroll_packets = 0U;
  instance->startup_preroll_first_pts_90k = STBP_PTS_NONE;
  instance->startup_preroll_last_pts_90k = STBP_PTS_NONE;
  instance->startup_sync_floor_pts_90k = STBP_PTS_NONE;
#if BCM_STARTUP_PREROLL_CATCHUP
  instance->startup_catchup_active = 0;
  instance->startup_catchup_target_pts_90k = STBP_PTS_NONE;
  instance->startup_catchup_deadline_ms = 0;
#endif
  return final_result;
}

static enum stbp_result bcm_create(const struct stbp_host_callbacks* host, void** output)
{
  struct bcm_instance* instance;

  if (host == NULL || host->struct_size < sizeof(*host) || output == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  instance = (struct bcm_instance*)calloc(1, sizeof(*instance));
  if (instance == NULL)
    return STBP_ERROR_BACKEND;
  instance->host = *host;
  instance->state = STBP_STATE_CLOSED;
  instance->video_fd = -1;
  instance->last_pts_90k = STBP_PTS_NONE;
  if (pthread_mutex_init(&instance->mutex, NULL) != 0)
  {
    free(instance);
    return STBP_ERROR_BACKEND;
  }
  *output = instance;
  return STBP_OK;
}

static void bcm_destroy(void* opaque)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  if (instance == NULL)
    return;
  (void)pthread_mutex_lock(&instance->mutex);
  (void)close_locked(instance);
  (void)pthread_mutex_unlock(&instance->mutex);
  (void)pthread_mutex_destroy(&instance->mutex);
  free(instance);
}

static enum stbp_result bcm_probe(void* opaque, struct stbp_capabilities* capabilities)
{
  (void)opaque;
  if (capabilities == NULL || capabilities->struct_size < sizeof(*capabilities))
    return STBP_ERROR_INVALID_ARGUMENT;
  if (access(BCM_VIDEO_DEVICE, R_OK | W_OK) != 0)
    return STBP_ERROR_NO_DEVICE;

  capabilities->codec_mask = STBP_CODEC_BIT(STBP_CODEC_MPEG1) |
                             STBP_CODEC_BIT(STBP_CODEC_MPEG2) |
                             STBP_CODEC_BIT(STBP_CODEC_MPEG4_PART2) |
                             STBP_CODEC_BIT(STBP_CODEC_MSMPEG4V3) |
                             STBP_CODEC_BIT(STBP_CODEC_DIVX4) |
                             STBP_CODEC_BIT(STBP_CODEC_DIVX5) |
                             STBP_CODEC_BIT(STBP_CODEC_XVID) |
                             STBP_CODEC_BIT(STBP_CODEC_H263) |
                             STBP_CODEC_BIT(STBP_CODEC_H264);
#if STBP_BCM_DVB_HAVE_HEVC
  capabilities->codec_mask |= STBP_CODEC_BIT(STBP_CODEC_HEVC);
#endif
#if STBP_BCM_DVB_HAVE_WMV
  capabilities->codec_mask |= STBP_CODEC_BIT(STBP_CODEC_VC1) |
                              STBP_CODEC_BIT(STBP_CODEC_WMV3);
#endif
#if STBP_BCM_DVB_HAVE_VP6
  capabilities->codec_mask |= STBP_CODEC_BIT(STBP_CODEC_VP6);
#endif
#if STBP_BCM_DVB_HAVE_VP8
  capabilities->codec_mask |= STBP_CODEC_BIT(STBP_CODEC_VP8);
#endif
#if STBP_BCM_DVB_HAVE_VP9
  capabilities->codec_mask |= STBP_CODEC_BIT(STBP_CODEC_VP9);
#endif
#if STBP_BCM_DVB_HAVE_SPARK
  capabilities->codec_mask |= STBP_CODEC_BIT(STBP_CODEC_FLV1);
#endif
  capabilities->feature_mask = STBP_FEATURE_PAUSE |
                               STBP_FEATURE_PRESENTATION_CLOCK |
                               STBP_FEATURE_DRAIN |
                               STBP_FEATURE_INTERLACED;
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
  capabilities->feature_mask |= STBP_FEATURE_STARTUP_PTS_GATE;
#endif
  capabilities->max_width = 4096;
  capabilities->max_height = 2160;
  capabilities->max_packet_size = BCM_MAX_PACKET_SIZE;
  capabilities->preferred_queue_bytes = BCM_PREFERRED_QUEUE_BYTES;
  return STBP_OK;
}

static enum stbp_result bcm_open(void* opaque,
                                 const struct stbp_stream_config* stream,
                                 const struct stbp_clock_config* clock)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  enum stbp_result final_result = STBP_ERROR_BACKEND;
  int stream_type;
  (void)clock;

  if (instance == NULL || stream == NULL || stream->struct_size < sizeof(*stream) ||
      (clock != NULL && clock->struct_size < sizeof(*clock)) ||
      (stream->extra_data == NULL && stream->extra_data_size != 0))
    return STBP_ERROR_INVALID_ARGUMENT;
  if ((stream->flags & STBP_STREAM_ENCRYPTED) != 0 ||
      (codec_uses_annex_b(stream->codec) && (stream->flags & STBP_STREAM_ANNEX_B) == 0) ||
      stream->extra_data_size > BCM_MAX_PACKET_SIZE)
    return STBP_ERROR_UNSUPPORTED;
  stream_type = codec_to_stream_type(stream->codec);
  if (stream_type < 0)
    return STBP_ERROR_UNSUPPORTED;
  /* BCM7251S advertises a 4K output surface, but its AVC decoder is limited
   * to HD. Sending UHD H.264 leaves both video and the shared A/V clock stuck. */
  if (stream->codec == STBP_CODEC_H264 && (stream->width > 1920U || stream->height > 1088U))
    return STBP_ERROR_UNSUPPORTED;
#if STBP_BCM_DVB_LIMITED_MPEG4V2
  if ((stream->codec == STBP_CODEC_MPEG4_PART2 ||
       stream->codec == STBP_CODEC_MSMPEG4V3 || stream->codec == STBP_CODEC_DIVX4 ||
       stream->codec == STBP_CODEC_DIVX5 || stream->codec == STBP_CODEC_XVID) &&
      (stream->width > 800U || stream->height > 600U))
    return STBP_ERROR_UNSUPPORTED;
#endif

  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_CLOSED)
  {
    final_result = STBP_ERROR_BAD_STATE;
    goto done;
  }
  if (!configure_codec_data(instance, stream))
    goto failed;

#if STBP_BCM_DVB_NEXUS_STC
  if (access(BCM_STC_HOST_DEVICE, W_OK) != 0 && !open_nexus_host_stc(instance))
    bcm_log(instance, STBP_LOG_WARNING, "Broadcom Nexus host STC is unavailable");
#endif

  instance->video_fd = open(BCM_VIDEO_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (instance->video_fd < 0)
  {
    bcm_log_errno(instance, STBP_LOG_ERROR, "opening " BCM_VIDEO_DEVICE);
    final_result = errno == EBUSY ? STBP_ERROR_BUSY : STBP_ERROR_NO_DEVICE;
    goto failed;
  }
  /* Several legacy Broadcom MIPSel drivers (notably BCM7425) retain the
   * previous Enigma2 decoder surface until the device buffer is cleared.
   * The TYPE2 xcore driver uses the reference E2 order without either
   * startup clear; clearing here prevents HEVC from starting on BCM73565. */
#if !defined(STBP_BCM_DVB_VARIANT_TYPE2) && \
    !defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
  if (ioctl(instance->video_fd, VIDEO_CLEAR_BUFFER) < 0)
    bcm_log_errno(instance, STBP_LOG_DEBUG, "initial VIDEO_CLEAR_BUFFER");
#endif
  if (ioctl(instance->video_fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY) < 0)
  {
    bcm_log_errno(instance, STBP_LOG_ERROR, "VIDEO_SELECT_SOURCE(memory)");
    goto failed;
  }
  if (ioctl(instance->video_fd, VIDEO_FREEZE) < 0)
    bcm_log_errno(instance, STBP_LOG_DEBUG, "initial VIDEO_FREEZE");
  set_fallback_framerate(&stream->frame_rate);
  if (ioctl(instance->video_fd, VIDEO_SET_STREAMTYPE, stream_type) < 0)
  {
    bcm_log_errno(instance, STBP_LOG_ERROR, "VIDEO_SET_STREAMTYPE");
    goto failed;
  }
#if defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
  /* This Nexus DVB driver independently selects one of its 24/50/60 Hz mode
   * aliases from the elementary-stream header after VIDEO_PLAY.  That can
   * override Kodi's container-PTS based choice (for example a 25 fps MKV with
   * a stale 23.976 SPS). Pin every alias to Kodi's current output for the
   * decoder lifetime; close_locked restores Enigma2's original mappings. */
  if (!pin_gigablue_video_modes(instance))
    bcm_log(instance, STBP_LOG_WARNING, "GigaBlue automatic HDMI modes could not be pinned");
#endif
  if (ioctl(instance->video_fd, VIDEO_PLAY) < 0)
  {
    bcm_log_errno(instance, STBP_LOG_ERROR, "VIDEO_PLAY");
    goto failed;
  }
  if (ioctl(instance->video_fd, VIDEO_CONTINUE) < 0)
  {
    bcm_log_errno(instance, STBP_LOG_ERROR, "VIDEO_CONTINUE");
    goto failed;
  }
  /* Keep the BCM7425 post-start reset, but do not disturb the TYPE2 decoder
   * after VIDEO_PLAY/VIDEO_CONTINUE. */
#if !defined(STBP_BCM_DVB_VARIANT_TYPE2) && \
    !defined(STBP_BCM_DVB_VARIANT_GIGABLUE)
  if (ioctl(instance->video_fd, VIDEO_CLEAR_BUFFER) < 0)
    bcm_log_errno(instance, STBP_LOG_DEBUG, "post-start VIDEO_CLEAR_BUFFER");
#endif

  instance->codec = stream->codec;
  instance->state = STBP_STATE_OPEN;
  instance->last_error = STBP_OK;
  instance->last_pts_90k = STBP_PTS_NONE;
  instance->stc_host_enabled = 0;
  instance->stc_needs_seed = host_stc_available(instance);
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
  instance->decoder_has_data = 0;
  instance->resume_startup_pending = 0;
#endif
  instance->startup_preroll = 1;
  instance->startup_preroll_packets = 0U;
  instance->startup_preroll_first_pts_90k = STBP_PTS_NONE;
  instance->startup_preroll_last_pts_90k = STBP_PTS_NONE;
  instance->startup_sync_floor_pts_90k = STBP_PTS_NONE;
#if BCM_STARTUP_PREROLL_CATCHUP
  instance->startup_catchup_active = 0;
  instance->startup_catchup_target_pts_90k = STBP_PTS_NONE;
  instance->startup_catchup_deadline_ms = 0;
#endif
  bcm_log(instance, STBP_LOG_INFO, "Broadcom Linux-DVB PES video path opened");
  final_result = STBP_OK;
  goto done;

failed:
  (void)close_locked(instance);
done:
  (void)pthread_mutex_unlock(&instance->mutex);
  return final_result;
}

static enum stbp_result bcm_queue_packet(void* opaque, const struct stbp_packet* packet)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  enum stbp_result result;
  int64_t pts;

  if (instance == NULL || packet == NULL || packet->struct_size < sizeof(*packet) ||
      (packet->data == NULL && packet->size != 0) || packet->size > BCM_MAX_PACKET_SIZE)
    return STBP_ERROR_INVALID_ARGUMENT;

  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN && instance->state != STBP_STATE_PAUSED)
  {
    result = STBP_ERROR_BAD_STATE;
    goto done;
  }
  result = flush_pending_locked(instance);
  if (result != STBP_OK)
    goto done;
  if ((packet->flags & STBP_PACKET_END_OF_STREAM) != 0 && packet->size == 0)
  {
    result = STBP_ERROR_UNSUPPORTED;
    goto done;
  }
  if ((packet->flags & STBP_PACKET_DISCONTINUITY) != 0)
  {
    if (ioctl(instance->video_fd, VIDEO_CLEAR_BUFFER) < 0)
    {
      bcm_log_errno(instance, STBP_LOG_ERROR, "VIDEO_CLEAR_BUFFER(discontinuity)");
      result = STBP_ERROR_IO;
      goto done;
    }
    instance->send_codec_data = instance->codec_data_size != 0;
    instance->stc_needs_seed = host_stc_available(instance);
  }

  pts = packet->pts_90k != STBP_PTS_NONE ? packet->pts_90k : packet->dts_90k;
  /* A file resume opens a fresh decoder and Kodi feeds the preceding GOP as
   * decode-only packets before the requested resume timestamp. Track that
   * startup-only GOP so the normal Broadcom driver can consume it in trick
   * mode without changing the already proven ordinary-seek path. */
  if (instance->startup_preroll && (packet->flags & STBP_PACKET_DROP) != 0)
  {
    if (instance->startup_preroll_packets == 0U)
      instance->startup_preroll_first_pts_90k = pts;
    instance->startup_preroll_last_pts_90k = pts;
    ++instance->startup_preroll_packets;
#if BCM_STARTUP_PREROLL_CATCHUP
    /* A single frame is within normal decoder startup tolerance. Enter trick
     * mode only after a real multi-frame GOP preroll has been confirmed.
     * Official Vu+ ARM drivers cannot expose their occupied Nexus STC channel;
     * without this temporary catch-up, a long resume GOP is decoded in real
     * time while Kodi audio already advances from the requested timestamp. */
    if (instance->startup_preroll_packets == 2U)
    {
      if (ioctl(instance->video_fd, VIDEO_FAST_FORWARD, 8) == 0)
      {
        (void)ioctl(instance->video_fd, VIDEO_CONTINUE);
        instance->startup_catchup_active = 1;
        bcm_log(instance, STBP_LOG_INFO,
                "Broadcom startup preroll catch-up enabled at 8x");
      }
      else
        bcm_log_errno(instance, STBP_LOG_DEBUG, "startup VIDEO_FAST_FORWARD");
    }
#endif
  }
  else if (instance->startup_preroll)
  {
    if (instance->startup_preroll_packets != 0U)
    {
      char message[224];
      (void)snprintf(message, sizeof(message),
                     "Broadcom startup preroll: %u decode-only packets, pts=%lld..%lld",
                     instance->startup_preroll_packets,
                     (long long)instance->startup_preroll_first_pts_90k,
                     (long long)instance->startup_preroll_last_pts_90k);
      bcm_log(instance, STBP_LOG_INFO, message);
    }
    instance->startup_sync_floor_pts_90k = pts;
#if BCM_STARTUP_PREROLL_CATCHUP
    if (instance->startup_catchup_active)
    {
      instance->startup_catchup_target_pts_90k = pts;
      instance->startup_catchup_deadline_ms = monotonic_milliseconds() + 2000U;
    }
#endif
    instance->startup_preroll = 0;
  }
  /* Keep accurate-seek preroll in the decoder so H.264/HEVC reference state
   * is rebuilt. Backends with a presentation-clock bridge decide when their
   * clock can be bootstrapped. Kodi's GENERAL_RESYNC later supplies audio and
   * video with one common clock after both stream players have entered their
   * synchronized state. */
#if STBP_BCM_DVB_NEXUS_STC
  /* A Nexus host channel starts frozen on receivers without the kernel STC
   * bridge.  Waiting exclusively for Kodi's GENERAL_RESYNC deadlocks startup:
   * Kodi waits for the first decoded picture while the decoder waits for a
   * running STC.  Bootstrap it from the first presentation packet; Kodi's
   * later GENERAL_RESYNC still replaces this value with the common A/V clock.
   * Accurate-seek preroll packets remain queued without being presented. */
  if (instance->stc_needs_seed && instance->nexus_stc_channel != NULL &&
      (packet->flags & STBP_PACKET_DROP) == 0 && pts != STBP_PTS_NONE &&
      !seed_host_stc(instance, pts))
    bcm_log(instance, STBP_LOG_WARNING, "Broadcom Nexus startup STC could not be seeded");
#endif
  result = codec_uses_bcmv(instance->codec)
               ? prepare_bcmv_packet_locked(instance, packet, pts)
               : prepare_standard_packet_locked(instance, packet, pts);
  if (result != STBP_OK)
    goto done;

  /* The packet is accepted once it is owned by this backend. A short write is
   * retained in pending and back-pressures Kodi through get_buffer_state. */
  result = flush_pending_locked(instance);
  if (result == STBP_AGAIN)
    result = STBP_OK;
  if (result == STBP_OK)
  {
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
    instance->decoder_has_data = 1;
#endif
    ++instance->packets_queued;
    if ((packet->flags & STBP_PACKET_DROP) != 0)
      ++instance->packets_dropped;
    if ((packet->flags & STBP_PACKET_DROP) == 0)
      instance->last_pts_90k = pts;
  }

done:
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result bcm_get_buffer_state(void* opaque, struct stbp_buffer_state* state)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  enum stbp_result result = STBP_OK;

  if (instance == NULL || state == NULL || state->struct_size < sizeof(*state))
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state == STBP_STATE_CLOSED || instance->state == STBP_STATE_ERROR)
  {
    result = STBP_ERROR_BAD_STATE;
    goto done;
  }
  if (instance->pending != NULL)
  {
    result = flush_pending_locked(instance);
    if (result == STBP_AGAIN)
      result = STBP_OK;
    else if (result != STBP_OK)
      goto done;
  }
  state->queued_bytes = (uint32_t)(instance->pending_size - instance->pending_offset);
  state->capacity_bytes = BCM_PREFERRED_QUEUE_BYTES;
  state->queued_packets = instance->pending != NULL ? 1U : 0U;
  state->can_accept_packet = instance->state == STBP_STATE_OPEN &&
                            instance->pending == NULL &&
                            fd_can_accept_packet(instance->video_fd);
done:
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result bcm_get_status(void* opaque, struct stbp_status* status)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  uint64_t decoder_pts = 0;

  if (instance == NULL || status == NULL || status->struct_size < sizeof(*status))
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  status->state = instance->state;
  status->last_error = instance->last_error;
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
  /* Dreambox exposes a useful decoder presentation PTS. Do not replace a
   * not-yet-started zero value with the last queued packet: Kodi uses the
   * distinction to defer its first post-resume A/V synchronization. */
  status->presentation_pts_90k = STBP_PTS_NONE;
  if (instance->video_fd >= 0 && ioctl(instance->video_fd, VIDEO_GET_PTS, &decoder_pts) == 0 &&
      decoder_pts != 0)
  {
    status->presentation_pts_90k = (int64_t)decoder_pts;
    if (instance->resume_startup_pending)
    {
      instance->resume_startup_pending = 0;
      bcm_log(instance, STBP_LOG_INFO,
              "Dreambox resume decoder PTS ready; normal pause handling restored");
    }
  }
#else
  status->presentation_pts_90k = instance->last_pts_90k;
  /* Some VU+ Broadcom drivers report a successful VIDEO_GET_PTS ioctl but
   * return zero for the complete memory-source session.  Zero is not useful
   * once a non-zero PES PTS has been queued: publishing it makes Kodi believe
   * that the hardware decoder never advances and upsets its frame scheduling.
   * Retain the last submitted PES timestamp in that case. */
  if (instance->video_fd >= 0 && ioctl(instance->video_fd, VIDEO_GET_PTS, &decoder_pts) == 0 &&
      (decoder_pts != 0 || instance->last_pts_90k == 0))
    status->presentation_pts_90k = (int64_t)decoder_pts;
#endif
#if BCM_STARTUP_PREROLL_CATCHUP
  if (instance->startup_catchup_active &&
      instance->startup_catchup_target_pts_90k != STBP_PTS_NONE)
  {
    const uint64_t now_ms = monotonic_milliseconds();
    const int caught_up = decoder_pts != 0 &&
                          (int64_t)decoder_pts + 4500 >=
                              instance->startup_catchup_target_pts_90k;
    const int timed_out = instance->startup_catchup_deadline_ms != 0 &&
                          now_ms != 0 && now_ms >= instance->startup_catchup_deadline_ms;
    if (caught_up)
      stop_startup_catchup_locked(instance,
                                  "Broadcom startup preroll catch-up complete");
    else if (timed_out)
      stop_startup_catchup_locked(instance,
                                  "Broadcom startup preroll catch-up timed out");
  }
#endif
  status->packets_queued = instance->packets_queued;
  status->packets_dropped = instance->packets_dropped;
  (void)pthread_mutex_unlock(&instance->mutex);
  return STBP_OK;
}

static enum stbp_result bcm_flush(void* opaque, int64_t next_pts_90k)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  enum stbp_result result = STBP_OK;
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
  int clear_decoder;
#endif

  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state == STBP_STATE_CLOSED)
    result = STBP_ERROR_BAD_STATE;
  else
  {
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
    /* Kodi seeks to a saved resume point immediately after opening a fresh
     * decoder. Dreambox BCM drivers can stop presenting permanently when an
     * empty decoder is cleared between VIDEO_PLAY and its first PES packet.
     * Once data has entered the decoder, ordinary seek flushes retain the
     * established VIDEO_CLEAR_BUFFER path. */
    clear_decoder = instance->decoder_has_data;
#endif
    release_pending(instance);
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
    if (!clear_decoder)
    {
      /* Keep the just-opened decoder completely untouched. Even nominally
       * harmless speed/continue ioctls before its first PES timestamp can
       * establish a wrong internal time mapping on Dreambox BCM drivers. */
      instance->state = STBP_STATE_OPEN;
      instance->send_codec_data = instance->codec_data_size != 0;
      instance->last_pts_90k = next_pts_90k;
      instance->stc_needs_seed = host_stc_available(instance);
      instance->decoder_has_data = 0;
      instance->resume_startup_pending = 1;
      bcm_log(instance, STBP_LOG_INFO,
              "Dreambox initial resume flush left the empty decoder untouched");
      goto flush_done;
    }
#endif
    if (ioctl(instance->video_fd, VIDEO_CLEAR_BUFFER) < 0)
    {
      bcm_log_errno(instance, STBP_LOG_ERROR, "VIDEO_CLEAR_BUFFER");
      result = STBP_ERROR_IO;
    }
    else
    {
#if BCM_STARTUP_PREROLL_CATCHUP
      stop_startup_catchup_locked(instance,
                                  "Broadcom startup preroll catch-up cancelled by flush");
#endif
      (void)ioctl(instance->video_fd, VIDEO_FAST_FORWARD, 0);
      (void)ioctl(instance->video_fd, VIDEO_SLOWMOTION, 0);
      (void)ioctl(instance->video_fd, VIDEO_CONTINUE);
      instance->state = STBP_STATE_OPEN;
      instance->send_codec_data = instance->codec_data_size != 0;
      instance->last_pts_90k = next_pts_90k;
      instance->stc_needs_seed = host_stc_available(instance);
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
      instance->decoder_has_data = 0;
      instance->resume_startup_pending = 0;
#endif
    }
  }
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
flush_done:
#endif
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result bcm_drain(void* opaque)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  enum stbp_result result;

  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN && instance->state != STBP_STATE_PAUSED)
    result = instance->state == STBP_STATE_DRAINING ? STBP_OK : STBP_ERROR_BAD_STATE;
  else
  {
    result = flush_pending_locked(instance);
    if (result == STBP_OK)
      instance->state = STBP_STATE_DRAINING;
  }
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result bcm_reset(void* opaque)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  enum stbp_result result;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  result = close_locked(instance);
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result bcm_sync_clock(void* opaque, int64_t pts_90k)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  enum stbp_result result = STBP_OK;
  int64_t sync_pts_90k = pts_90k;

  if (instance == NULL || pts_90k == STBP_PTS_NONE)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN && instance->state != STBP_STATE_PAUSED)
    result = STBP_ERROR_BAD_STATE;
  else if (!host_stc_available(instance))
    result = STBP_ERROR_UNSUPPORTED;
  else
  {
    /* Kodi's first common-clock value after a file resume can precede the
     * first displayable video PTS by the audio decoder's startup preroll. Do
     * not move an already bootstrapped hardware clock backwards across that
     * first picture; doing so starts audio while Broadcom waits to present it.
     * The floor is stream-derived and consumed once, so normal seek clock
     * synchronization is unaffected. */
    if (instance->startup_sync_floor_pts_90k != STBP_PTS_NONE)
    {
      if (sync_pts_90k < instance->startup_sync_floor_pts_90k)
      {
        char message[192];
        (void)snprintf(message, sizeof(message),
                       "Broadcom startup clock clamped: request=%lld, first-video=%lld",
                       (long long)sync_pts_90k,
                       (long long)instance->startup_sync_floor_pts_90k);
        bcm_log(instance, STBP_LOG_INFO, message);
        sync_pts_90k = instance->startup_sync_floor_pts_90k;
      }
      instance->startup_sync_floor_pts_90k = STBP_PTS_NONE;
    }
    if (!seed_host_stc(instance, sync_pts_90k))
      result = STBP_ERROR_IO;
  }
  instance->last_error = result;
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result bcm_set_speed(void* opaque, struct stbp_rational speed)
{
  (void)opaque;
  if (speed.denominator <= 0)
    return STBP_ERROR_INVALID_ARGUMENT;
  return speed.numerator == speed.denominator ? STBP_OK : STBP_ERROR_UNSUPPORTED;
}

static enum stbp_result bcm_set_paused(void* opaque, int paused)
{
  struct bcm_instance* instance = (struct bcm_instance*)opaque;
  enum stbp_result result = STBP_OK;

  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN && instance->state != STBP_STATE_PAUSED)
    result = STBP_ERROR_BAD_STATE;
#if defined(STBP_BCM_DVB_VARIANT_DREAMBOX)
  else if (paused && instance->resume_startup_pending)
  {
    /* Kodi pauses both players while filling its startup cache. Applying that
     * pause before Dream's first decoder PTS creates a circular wait: Kodi
     * waits for a picture while the decoder waits for Kodi to resume. Keep
     * decoding until GetStatus observes the first valid hardware PTS. */
    bcm_log(instance, STBP_LOG_INFO,
            "Dreambox resume startup pause deferred until decoder PTS");
  }
#endif
  else if (ioctl(instance->video_fd, paused ? VIDEO_FREEZE : VIDEO_CONTINUE) < 0)
  {
    bcm_log_errno(instance, STBP_LOG_ERROR, paused ? "VIDEO_FREEZE" : "VIDEO_CONTINUE");
    result = STBP_ERROR_IO;
  }
  else
  {
    instance->state = paused ? STBP_STATE_PAUSED : STBP_STATE_OPEN;
    if (instance->stc_host_enabled)
    {
#if STBP_BCM_DVB_NEXUS_STC
      if (instance->nexus_stc_channel != NULL && instance->nexus_stc_freeze != NULL)
        (void)instance->nexus_stc_freeze(instance->nexus_stc_channel, paused != 0);
      else
#endif
        (void)write_stc_command(instance, paused ? "freeze 1" : "freeze 0");
    }
  }
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result bcm_set_video_rect(void* opaque,
                                           const struct stbp_rect* source,
                                           const struct stbp_rect* destination)
{
  (void)opaque;
  (void)source;
  (void)destination;
  return STBP_ERROR_UNSUPPORTED;
}

static enum stbp_result bcm_set_visible(void* opaque, int visible)
{
  (void)opaque;
  (void)visible;
  return STBP_ERROR_UNSUPPORTED;
}

static enum stbp_result bcm_close(void* opaque)
{
  return bcm_reset(opaque);
}

static const struct stbp_backend_api_v1 bcm_api = {
    STBP_ABI_VERSION_1,
    sizeof(struct stbp_backend_api_v1),
    "bcm-dvb",
    "0.2.1-vp9-segmented",
    bcm_create,
    bcm_destroy,
    bcm_probe,
    bcm_open,
    bcm_queue_packet,
    bcm_get_buffer_state,
    bcm_get_status,
    bcm_flush,
    bcm_drain,
    bcm_reset,
    bcm_sync_clock,
    bcm_set_speed,
    bcm_set_paused,
    bcm_set_video_rect,
    bcm_set_visible,
    bcm_close};

STBP_EXPORT const struct stbp_backend_api_v1* stbp_backend_get_api(uint32_t host_abi_version,
                                                                   uint32_t host_api_size)
{
  if (host_abi_version != STBP_ABI_VERSION_1 || host_api_size < sizeof(bcm_api))
    return NULL;
  return &bcm_api;
}
