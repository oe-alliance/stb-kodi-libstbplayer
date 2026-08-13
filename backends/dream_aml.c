/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DreamOne/DreamTwo native Amlogic video backend.
 *
 * Dream's closed dreamvideosink does not expose decoded V4L2 frames. It feeds
 * access units through a private Linux-DVB ioctl and renders the decoder
 * output directly through the Amlogic amvideo plane. Kodi remains responsible
 * for demuxing, audio, clocks, subtitles and player control; RendererSTB only
 * controls the hardware plane's rectangle and visibility.
 */
#include "stbplayer/backend.h"

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
#include <unistd.h>

#define DREAM_VIDEO_DEVICE "/dev/dvb/adapter0/video0"
#define DREAM_POLL_DEVICE "/dev/amvideo_poll"
#define DREAM_VFM_MAP "/sys/class/vfm/map"
#define DREAM_VIDEO_AXIS "/sys/class/video/axis"
#define DREAM_VIDEO_SCREEN_MODE "/sys/class/video/screen_mode"
#define DREAM_DISABLE_VIDEO "/sys/class/video/disable_video"
#define DREAM_FREERUN_MODE "/sys/class/video/freerun_mode"
#define DREAM_TSYNC_ENABLE "/sys/class/tsync/enable"

#define DREAM_MAX_PACKET_SIZE (16U * 1024U * 1024U)
#define DREAM_QUEUE_BYTES (4U * 1024U * 1024U)
#define DREAM_STREAMTYPE_FROM_SYSINFO (-1)

#define DREAM_DEC_MPEG12 0U
#define DREAM_DEC_MPEG4 3U
#define DREAM_DEC_H264 4U
#define DREAM_DEC_MJPEG 5U
#define DREAM_DEC_H263 7U
#define DREAM_DEC_WMV3 10U
#define DREAM_DEC_WVC1 11U
#define DREAM_DEC_AVS 13U
#define DREAM_DEC_HEVC 15U
#define DREAM_DEC_VP9 16U
#define DREAM_DEC_AVS2 17U

struct dream_video_frame
{
  uint64_t pts;
  ssize_t bytes[8];
  const uint8_t* data[8];
  int is_phys_addr[8];
};

struct dream_video_dec_sysinfo
{
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t rate;
  uint32_t extra;
  uint32_t status;
  uint32_t ratio;
  uint32_t padding;
  uint64_t ratio64;
  void* param;
};

_Static_assert(sizeof(struct dream_video_frame) == 0xa8,
               "Dream VIDEO_SET_FRAME ABI mismatch");
_Static_assert(sizeof(struct dream_video_dec_sysinfo) == 0x30,
               "Dream VIDEO_SET_DEC_SYSINFO ABI mismatch");

#define DREAM_VIDEO_SET_FRAME _IOWR('o', 64, struct dream_video_frame)
#define DREAM_VIDEO_SET_DEC_SYSINFO _IOWR('o', 65, struct dream_video_dec_sysinfo)
#define DREAM_VIDEO_GET_FRAMERATE _IOR('o', 56, uint32_t)

struct dream_codec_info
{
  uint32_t decoder_format;
  int stream_type;
};

struct dream_instance
{
  struct stbp_host_callbacks host;
  pthread_mutex_t mutex;
  enum stbp_state state;
  enum stbp_result last_error;
  enum stbp_codec codec;
  int video_fd;
  int poll_fd;
  struct dream_video_dec_sysinfo sysinfo;
  int stream_type;
  struct stbp_rational frame_rate;
  uint8_t* codec_data;
  size_t codec_data_size;
  int send_codec_data;
  int saved_disable_video;
  int saved_freerun_mode;
  int saved_tsync_enable;
  int restore_disable_video;
  int restore_freerun_mode;
  int restore_tsync_enable;
  int visible;
  uint64_t packets_queued;
  uint64_t packets_dropped;
  int64_t last_pts_90k;
  int first_packet_logged;
  int clock_mode_configured;
  int video_only;
};

static void dream_log(struct dream_instance* instance,
                      enum stbp_log_level level,
                      const char* message)
{
  if (instance != NULL && instance->host.log != NULL)
    instance->host.log(instance->host.userdata, level, message);
}

static void dream_log_errno(struct dream_instance* instance,
                            enum stbp_log_level level,
                            const char* operation)
{
  char message[256];
  const int saved_errno = errno;
  (void)snprintf(message, sizeof(message), "%s failed: %s (%d)", operation,
                 strerror(saved_errno), saved_errno);
  dream_log(instance, level, message);
}

static int write_text(const char* path, const char* value)
{
  const size_t length = strlen(value);
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  ssize_t written;
  if (fd < 0)
    return -1;
  written = write(fd, value, length);
  (void)close(fd);
  return written == (ssize_t)length ? 0 : -1;
}

static int write_number(const char* path, int value)
{
  char text[32];
  const int length = snprintf(text, sizeof(text), "%d", value);
  return length > 0 && (size_t)length < sizeof(text) ? write_text(path, text) : -1;
}

static int read_number(const char* path, int* value)
{
  char text[64];
  char* end = NULL;
  int fd;
  ssize_t length;
  long parsed;
  if (value == NULL)
    return -1;
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  length = read(fd, text, sizeof(text) - 1U);
  (void)close(fd);
  if (length <= 0)
    return -1;
  text[length] = '\0';
  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno != 0 || end == text)
    return -1;
  *value = (int)parsed;
  return 0;
}

static void set_direct_vfm_route(void)
{
  (void)write_text(DREAM_VFM_MAP, "rm default");
  (void)write_text(DREAM_VFM_MAP,
                   "add default decoder ppmgr deinterlace amvideo");
}

static int codec_to_dream(enum stbp_codec codec, struct dream_codec_info* info)
{
  if (info == NULL)
    return -1;
  switch (codec)
  {
    case STBP_CODEC_MPEG1:
    case STBP_CODEC_MPEG2:
      info->decoder_format = DREAM_DEC_MPEG12;
      info->stream_type = 0;
      return 0;
    case STBP_CODEC_MPEG4_PART2:
    case STBP_CODEC_MSMPEG4V3:
    case STBP_CODEC_DIVX4:
    case STBP_CODEC_DIVX5:
    case STBP_CODEC_XVID:
      info->decoder_format = DREAM_DEC_MPEG4;
      info->stream_type = 4;
      return 0;
    case STBP_CODEC_H263:
      info->decoder_format = DREAM_DEC_H263;
      info->stream_type = 4;
      return 0;
    case STBP_CODEC_H264:
      info->decoder_format = DREAM_DEC_H264;
      info->stream_type = 1;
      return 0;
    case STBP_CODEC_HEVC:
      info->decoder_format = DREAM_DEC_HEVC;
      info->stream_type = 22;
      return 0;
    case STBP_CODEC_VC1:
      info->decoder_format = DREAM_DEC_WVC1;
      info->stream_type = 3;
      return 0;
    case STBP_CODEC_WMV3:
      info->decoder_format = DREAM_DEC_WMV3;
      info->stream_type = 3;
      return 0;
    case STBP_CODEC_VP9:
      info->decoder_format = DREAM_DEC_VP9;
      info->stream_type = 23;
      return 0;
    case STBP_CODEC_MJPEG:
      info->decoder_format = DREAM_DEC_MJPEG;
      info->stream_type = 5;
      return 0;
    case STBP_CODEC_AVS:
      info->decoder_format = DREAM_DEC_AVS;
      info->stream_type = DREAM_STREAMTYPE_FROM_SYSINFO;
      return 0;
    case STBP_CODEC_AVS2:
      info->decoder_format = DREAM_DEC_AVS2;
      info->stream_type = DREAM_STREAMTYPE_FROM_SYSINFO;
      return 0;
    default:
      return -1;
  }
}

static size_t find_h264_nal(const uint8_t* data, size_t size, unsigned int type, size_t start)
{
  size_t offset;
  if (data == NULL || size < 4U)
    return size;
  for (offset = start; offset + 3U < size; ++offset)
  {
    size_t header;
    if (data[offset] != 0U || data[offset + 1U] != 0U)
      continue;
    if (data[offset + 2U] == 1U)
      header = offset + 3U;
    else if (offset + 4U < size && data[offset + 2U] == 0U && data[offset + 3U] == 1U)
      header = offset + 4U;
    else
      continue;
    if (header < size && (data[header] & 0x1fU) == type)
      return offset;
  }
  return size;
}

static size_t find_hevc_vcl_after_parameter_sets(const uint8_t* data, size_t size)
{
  size_t offset = 0;
  int have_vps = 0;
  int have_sps = 0;
  int have_pps = 0;
  while (data != NULL && offset + 3U < size)
  {
    size_t header;
    unsigned int type;
    if (data[offset] != 0U || data[offset + 1U] != 0U)
    {
      ++offset;
      continue;
    }
    if (data[offset + 2U] == 1U)
      header = offset + 3U;
    else if (offset + 4U < size && data[offset + 2U] == 0U && data[offset + 3U] == 1U)
      header = offset + 4U;
    else
    {
      ++offset;
      continue;
    }
    if (header >= size)
      break;
    type = (data[header] >> 1U) & 0x3fU;
    if (type <= 31U)
      return have_vps && have_sps && have_pps ? offset : 0U;
    if (type == 32U)
      have_vps = 1;
    else if (type == 33U)
      have_sps = 1;
    else if (type == 34U)
      have_pps = 1;
    offset = header + 1U;
  }
  return 0U;
}

static void write_vp9_amlv_header(uint8_t* output, size_t frame_size)
{
  const uint32_t tagged_size = (uint32_t)frame_size + 4U;
  output[0] = (uint8_t)(tagged_size >> 24U);
  output[1] = (uint8_t)(tagged_size >> 16U);
  output[2] = (uint8_t)(tagged_size >> 8U);
  output[3] = (uint8_t)tagged_size;
  output[4] = output[0] ^ 0xffU;
  output[5] = output[1] ^ 0xffU;
  output[6] = output[2] ^ 0xffU;
  output[7] = output[3] ^ 0xffU;
  output[8] = 0U;
  output[9] = 0U;
  output[10] = 0U;
  output[11] = 1U;
  output[12] = 'A';
  output[13] = 'M';
  output[14] = 'L';
  output[15] = 'V';
}

static enum stbp_result prepare_vp9_packet(const uint8_t* data,
                                           size_t size,
                                           uint8_t** output,
                                           size_t* output_size)
{
  size_t frame_sizes[8];
  size_t frame_count = 1U;
  size_t payload_size = size;
  size_t required;
  size_t input_cursor = 0U;
  size_t output_cursor = 0U;
  uint8_t marker;
  uint8_t* prepared;

  if (data == NULL || size == 0U || output == NULL || output_size == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;

  frame_sizes[0] = size;
  marker = data[size - 1U];
  if ((marker & 0xe0U) == 0xc0U)
  {
    const size_t candidate_count = (marker & 0x07U) + 1U;
    const size_t magnitude = ((marker >> 3U) & 0x03U) + 1U;
    const size_t index_size = 2U + magnitude * candidate_count;
    size_t index_cursor;
    size_t total = 0U;
    size_t frame;

    if (index_size <= size && data[size - index_size] == marker)
    {
      index_cursor = size - index_size + 1U;
      for (frame = 0U; frame < candidate_count; ++frame)
      {
        size_t byte;
        size_t frame_size = 0U;
        for (byte = 0U; byte < magnitude; ++byte)
          frame_size |= (size_t)data[index_cursor++] << (byte * 8U);
        if (frame_size > size - index_size - total)
          break;
        frame_sizes[frame] = frame_size;
        total += frame_size;
      }
      if (frame == candidate_count && total <= size - index_size)
      {
        frame_count = candidate_count;
        payload_size = total;
      }
    }
  }

  if (payload_size > SIZE_MAX - frame_count * 16U)
    return STBP_ERROR_UNSUPPORTED;
  required = payload_size + frame_count * 16U;
  prepared = (uint8_t*)malloc(required);
  if (prepared == NULL)
    return STBP_ERROR_BACKEND;

  for (size_t frame = 0U; frame < frame_count; ++frame)
  {
    const size_t frame_size = frame_sizes[frame];
    if (frame_size > UINT32_MAX - 4U)
    {
      free(prepared);
      return STBP_ERROR_UNSUPPORTED;
    }
    write_vp9_amlv_header(prepared + output_cursor, frame_size);
    output_cursor += 16U;
    memcpy(prepared + output_cursor, data + input_cursor, frame_size);
    output_cursor += frame_size;
    input_cursor += frame_size;
  }

  *output = prepared;
  *output_size = output_cursor;
  return STBP_OK;
}

static enum stbp_result prepare_packet_locked(struct dream_instance* instance,
                                              const struct stbp_packet* packet,
                                              uint8_t** output,
                                              size_t* output_size)
{
  const uint8_t* data = packet->data;
  const size_t size = packet->size;
  size_t prefix_size = instance->send_codec_data ? instance->codec_data_size : 0U;
  size_t packet_offset = 0U;
  size_t sei = size;
  size_t sps = size;
  size_t pps = size;
  size_t idr = size;
  size_t required;
  uint8_t* prepared;
  size_t cursor = 0;
  int normalize_h264 = 0;

  if (instance->codec == STBP_CODEC_VP9)
    return prepare_vp9_packet(data, size, output, output_size);

  if (instance->codec == STBP_CODEC_H264)
  {
    sei = find_h264_nal(data, size, 6U, 0U);
    sps = find_h264_nal(data, size, 7U, 0U);
    pps = find_h264_nal(data, size, 8U, sps < size ? sps : 0U);
    idr = find_h264_nal(data, size, 5U, pps < size ? pps : 0U);
    normalize_h264 = sei == 0U && sps > sei && pps > sps && idr > pps && idr < size;
    if (sps < size && pps < size)
      prefix_size = 0U;
  }
  else if (instance->codec == STBP_CODEC_HEVC && prefix_size != 0U)
  {
    /* Kodi's hvc1/hev1 converter inserts VPS/SPS/PPS before the first IDR. The
     * Dream sink replaces that generated prefix with every hvcC array, including
     * Prefix-SEI. Keep the same ordering instead of duplicating the parameter sets. */
    packet_offset = find_hevc_vcl_after_parameter_sets(data, size);
  }

  required = prefix_size + (size - packet_offset) +
             (normalize_h264 && idr + 2U < size && data[idr] == 0U &&
                      data[idr + 1U] == 0U && data[idr + 2U] == 1U
                  ? 1U
                  : 0U);
  prepared = (uint8_t*)malloc(required == 0U ? 1U : required);
  if (prepared == NULL)
    return STBP_ERROR_BACKEND;

  if (prefix_size != 0U)
  {
    memcpy(prepared + cursor, instance->codec_data, prefix_size);
    cursor += prefix_size;
  }
  if (normalize_h264)
  {
    memcpy(prepared + cursor, data + sps, idr - sps);
    cursor += idr - sps;
    memcpy(prepared + cursor, data, sps);
    cursor += sps;
    if (idr + 2U < size && data[idr] == 0U && data[idr + 1U] == 0U &&
        data[idr + 2U] == 1U)
      prepared[cursor++] = 0U;
    memcpy(prepared + cursor, data + idr, size - idr);
    cursor += size - idr;
  }
  else if (size != 0U)
  {
    memcpy(prepared + cursor, data + packet_offset, size - packet_offset);
    cursor += size - packet_offset;
  }

  *output = prepared;
  *output_size = cursor;
  return STBP_OK;
}

static enum stbp_result start_decoder_locked(struct dream_instance* instance)
{
  video_size_t size;
  uint32_t frame_rate = 0;
  memset(&size, 0, sizeof(size));
  (void)ioctl(instance->video_fd, VIDEO_GET_SIZE, &size);
  (void)ioctl(instance->video_fd, DREAM_VIDEO_GET_FRAMERATE, &frame_rate);
  if (ioctl(instance->video_fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY) < 0 ||
      ioctl(instance->video_fd, VIDEO_FREEZE, 0) < 0 ||
      ioctl(instance->video_fd, DREAM_VIDEO_SET_DEC_SYSINFO, &instance->sysinfo) < 0)
  {
    dream_log_errno(instance, STBP_LOG_ERROR, "Dream decoder setup");
    return STBP_ERROR_IO;
  }
  if (instance->stream_type != DREAM_STREAMTYPE_FROM_SYSINFO &&
      ioctl(instance->video_fd, VIDEO_SET_STREAMTYPE, instance->stream_type) < 0)
  {
    dream_log_errno(instance, STBP_LOG_ERROR, "VIDEO_SET_STREAMTYPE");
    return STBP_ERROR_IO;
  }
  if (ioctl(instance->video_fd, VIDEO_PLAY, 0) < 0 ||
      ioctl(instance->video_fd, VIDEO_SLOWMOTION, 0) < 0 ||
      ioctl(instance->video_fd, VIDEO_FAST_FORWARD, 0) < 0 ||
      ioctl(instance->video_fd, VIDEO_CONTINUE, 0) < 0)
  {
    dream_log_errno(instance, STBP_LOG_ERROR, "starting Dream decoder");
    return STBP_ERROR_IO;
  }
  return STBP_OK;
}

static void release_codec_data(struct dream_instance* instance)
{
  free(instance->codec_data);
  instance->codec_data = NULL;
  instance->codec_data_size = 0;
  instance->send_codec_data = 0;
}

static enum stbp_result close_locked(struct dream_instance* instance)
{
  enum stbp_result result = STBP_OK;
  if (instance->state == STBP_STATE_CLOSED && instance->video_fd < 0 &&
      instance->poll_fd < 0)
  {
    release_codec_data(instance);
    return STBP_OK;
  }
  if (instance->video_fd >= 0)
  {
    (void)write_number(DREAM_DISABLE_VIDEO, 1);
    (void)ioctl(instance->video_fd, VIDEO_FREEZE, 0);
    if (ioctl(instance->video_fd, VIDEO_STOP, 0) < 0 && errno != EINVAL)
    {
      dream_log_errno(instance, STBP_LOG_WARNING, "VIDEO_STOP");
      result = STBP_ERROR_IO;
    }
    (void)ioctl(instance->video_fd, VIDEO_SLOWMOTION, 0);
    (void)ioctl(instance->video_fd, VIDEO_FAST_FORWARD, 0);
    (void)ioctl(instance->video_fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX);
  }
  if (instance->poll_fd >= 0)
  {
    (void)close(instance->poll_fd);
    instance->poll_fd = -1;
  }
  if (instance->video_fd >= 0)
  {
    (void)close(instance->video_fd);
    instance->video_fd = -1;
  }

  set_direct_vfm_route();
  if (instance->restore_freerun_mode)
    (void)write_number(DREAM_FREERUN_MODE, instance->saved_freerun_mode);
  if (instance->restore_tsync_enable)
    (void)write_number(DREAM_TSYNC_ENABLE, instance->saved_tsync_enable);
  if (instance->restore_disable_video)
    (void)write_number(DREAM_DISABLE_VIDEO, instance->saved_disable_video);

  release_codec_data(instance);
  instance->state = STBP_STATE_CLOSED;
  instance->last_error = result;
  instance->codec = STBP_CODEC_UNKNOWN;
  instance->last_pts_90k = STBP_PTS_NONE;
  instance->visible = 0;
  return result;
}

static enum stbp_result dream_create(const struct stbp_host_callbacks* host, void** output)
{
  struct dream_instance* instance;
  if (host == NULL || host->struct_size < sizeof(*host) || output == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  instance = (struct dream_instance*)calloc(1, sizeof(*instance));
  if (instance == NULL)
    return STBP_ERROR_BACKEND;
  instance->host = *host;
  instance->state = STBP_STATE_CLOSED;
  instance->video_fd = -1;
  instance->poll_fd = -1;
  instance->last_pts_90k = STBP_PTS_NONE;
  if (pthread_mutex_init(&instance->mutex, NULL) != 0)
  {
    free(instance);
    return STBP_ERROR_BACKEND;
  }
  *output = instance;
  return STBP_OK;
}

static void dream_destroy(void* opaque)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  if (instance == NULL)
    return;
  (void)pthread_mutex_lock(&instance->mutex);
  (void)close_locked(instance);
  (void)pthread_mutex_unlock(&instance->mutex);
  (void)pthread_mutex_destroy(&instance->mutex);
  free(instance);
}

static enum stbp_result dream_probe(void* opaque, struct stbp_capabilities* capabilities)
{
  (void)opaque;
  if (capabilities == NULL || capabilities->struct_size < sizeof(*capabilities))
    return STBP_ERROR_INVALID_ARGUMENT;
  if (access(DREAM_VIDEO_DEVICE, R_OK | W_OK) != 0 ||
      access(DREAM_POLL_DEVICE, R_OK | W_OK) != 0 || access(DREAM_VFM_MAP, W_OK) != 0)
    return STBP_ERROR_NO_DEVICE;
  capabilities->codec_mask = STBP_CODEC_BIT(STBP_CODEC_MPEG1) |
                             STBP_CODEC_BIT(STBP_CODEC_MPEG2) |
                             STBP_CODEC_BIT(STBP_CODEC_MPEG4_PART2) |
                             STBP_CODEC_BIT(STBP_CODEC_MSMPEG4V3) |
                             STBP_CODEC_BIT(STBP_CODEC_DIVX4) |
                             STBP_CODEC_BIT(STBP_CODEC_DIVX5) |
                             STBP_CODEC_BIT(STBP_CODEC_XVID) |
                             STBP_CODEC_BIT(STBP_CODEC_H263) |
                             STBP_CODEC_BIT(STBP_CODEC_H264) |
                             STBP_CODEC_BIT(STBP_CODEC_HEVC) |
                             STBP_CODEC_BIT(STBP_CODEC_VC1) |
                             STBP_CODEC_BIT(STBP_CODEC_WMV3) |
                             STBP_CODEC_BIT(STBP_CODEC_VP9) |
                             STBP_CODEC_BIT(STBP_CODEC_MJPEG) |
                             STBP_CODEC_BIT(STBP_CODEC_AVS) |
                             STBP_CODEC_BIT(STBP_CODEC_AVS2);
  capabilities->feature_mask = STBP_FEATURE_PAUSE | STBP_FEATURE_VIDEO_RECT |
                               STBP_FEATURE_VISIBILITY |
                               STBP_FEATURE_PRESENTATION_CLOCK |
                               STBP_FEATURE_DRAIN | STBP_FEATURE_INTERLACED;
  capabilities->max_width = 4096;
  capabilities->max_height = 2160;
  capabilities->max_packet_size = DREAM_MAX_PACKET_SIZE;
  capabilities->preferred_queue_bytes = DREAM_QUEUE_BYTES;
  return STBP_OK;
}

static enum stbp_result dream_open(void* opaque,
                                   const struct stbp_stream_config* stream,
                                   const struct stbp_clock_config* clock)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  struct dream_codec_info codec_info;
  uint64_t rate;
  enum stbp_result result = STBP_ERROR_BACKEND;
  (void)clock;
  if (instance == NULL || stream == NULL || stream->struct_size < sizeof(*stream) ||
      (clock != NULL && clock->struct_size < sizeof(*clock)) ||
      (stream->extra_data == NULL && stream->extra_data_size != 0U))
    return STBP_ERROR_INVALID_ARGUMENT;
  if ((stream->flags & STBP_STREAM_ENCRYPTED) != 0U ||
      ((stream->codec == STBP_CODEC_H264 || stream->codec == STBP_CODEC_HEVC) &&
       (stream->flags & STBP_STREAM_ANNEX_B) == 0U) ||
      codec_to_dream(stream->codec, &codec_info) != 0 ||
      stream->extra_data_size > DREAM_MAX_PACKET_SIZE)
    return STBP_ERROR_UNSUPPORTED;

  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_CLOSED)
  {
    result = STBP_ERROR_BAD_STATE;
    goto done;
  }
  if (stream->extra_data_size != 0U)
  {
    instance->codec_data = (uint8_t*)malloc(stream->extra_data_size);
    if (instance->codec_data == NULL)
      goto failed;
    memcpy(instance->codec_data, stream->extra_data, stream->extra_data_size);
    instance->codec_data_size = stream->extra_data_size;
    instance->send_codec_data = 1;
  }

  instance->restore_disable_video =
      read_number(DREAM_DISABLE_VIDEO, &instance->saved_disable_video) == 0;
  instance->restore_freerun_mode =
      read_number(DREAM_FREERUN_MODE, &instance->saved_freerun_mode) == 0;
  instance->restore_tsync_enable =
      read_number(DREAM_TSYNC_ENABLE, &instance->saved_tsync_enable) == 0;

  instance->video_fd = open(DREAM_VIDEO_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  instance->poll_fd = open(DREAM_POLL_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (instance->video_fd < 0 || instance->poll_fd < 0)
  {
    dream_log_errno(instance, STBP_LOG_ERROR, "opening Dream video devices");
    goto failed;
  }

  memset(&instance->sysinfo, 0, sizeof(instance->sysinfo));
  instance->sysinfo.format = codec_info.decoder_format;
  instance->sysinfo.width = stream->width;
  instance->sysinfo.height = stream->height;
  if (stream->codec == STBP_CODEC_HEVC)
    instance->sysinfo.height = (stream->height + 7U) & ~7U;
  if (stream->frame_rate.numerator > 0 && stream->frame_rate.denominator > 0)
  {
    rate = UINT64_C(96000) * (uint32_t)stream->frame_rate.denominator /
           (uint32_t)stream->frame_rate.numerator;
    instance->sysinfo.rate = (uint32_t)rate;
  }
  else
    instance->sysinfo.rate = 3203U;
  instance->stream_type = codec_info.stream_type;
  instance->frame_rate = stream->frame_rate;
  instance->codec = stream->codec;

  set_direct_vfm_route();
  (void)write_number(DREAM_DISABLE_VIDEO, 1);
  /* The fresh Dream decoder does not report writable before either an audio
   * clock or the video-only freerun clock exists.  Bootstrap in freerun; the
   * first access unit selects the final video-only or A/V clock before it is
   * submitted, so normal A/V still uses the native timestamp-paced mode. */
  (void)write_number(DREAM_TSYNC_ENABLE, 0);
  (void)write_number(DREAM_FREERUN_MODE, 1);
  result = start_decoder_locked(instance);
  if (result != STBP_OK)
    goto failed;

  instance->state = STBP_STATE_OPEN;
  instance->last_error = STBP_OK;
  instance->last_pts_90k = STBP_PTS_NONE;
  instance->clock_mode_configured = 0;
  instance->video_only = 0;
  dream_log(instance, STBP_LOG_INFO,
            "Dream native decoder opened with direct amvideo output");
  result = STBP_OK;
  goto done;

failed:
  (void)close_locked(instance);
done:
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result dream_queue_packet(void* opaque, const struct stbp_packet* packet)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  struct dream_video_frame frame;
  struct pollfd descriptors[2];
  struct video_event event;
  uint8_t* prepared = NULL;
  size_t prepared_size = 0;
  size_t offset = 0;
  int64_t pts;
  enum stbp_result result;

  if (instance == NULL || packet == NULL || packet->struct_size < sizeof(*packet) ||
      (packet->data == NULL && packet->size != 0U))
    return STBP_ERROR_INVALID_ARGUMENT;
  if (packet->size == 0U)
    return STBP_OK;
  if (packet->size > DREAM_MAX_PACKET_SIZE)
    return STBP_ERROR_UNSUPPORTED;

  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN)
  {
    result = instance->state == STBP_STATE_PAUSED ? STBP_AGAIN : STBP_ERROR_BAD_STATE;
    goto done;
  }
  {
    const int video_only = (packet->flags & STBP_PACKET_VIDEO_ONLY) != 0U;
    if (!instance->clock_mode_configured || instance->video_only != video_only)
    {
      if (video_only)
      {
        (void)write_number(DREAM_TSYNC_ENABLE, 0);
        (void)write_number(DREAM_FREERUN_MODE, 1);
        dream_log(instance, STBP_LOG_INFO,
                  "Dream decoder using video-only freerun clock");
      }
      else
      {
        (void)write_number(DREAM_FREERUN_MODE, 0);
        (void)write_number(DREAM_TSYNC_ENABLE, 1);
        dream_log(instance, STBP_LOG_INFO,
                  "Dream decoder using native audio/video clock");
      }
      instance->video_only = video_only;
      instance->clock_mode_configured = 1;
    }
  }
  result = prepare_packet_locked(instance, packet, &prepared, &prepared_size);
  if (result != STBP_OK)
    goto done;

  memset(&frame, 0, sizeof(frame));
  frame.pts = UINT64_MAX;
  pts = packet->pts_90k != STBP_PTS_NONE ? packet->pts_90k : packet->dts_90k;
  if (pts != STBP_PTS_NONE && pts >= 0 &&
      (instance->codec != STBP_CODEC_MPEG1 && instance->codec != STBP_CODEC_MPEG2))
    /* Dream's sink converts GstClockTime to the decoder's native 90 kHz domain
     * before VIDEO_SET_FRAME. VIDEO_GET_PTS performs the inverse conversion. */
    frame.pts = (uint64_t)pts;
  else if (pts != STBP_PTS_NONE && pts >= 0)
  {
    /* The proprietary Dream sink timestamps MPEG-1/2 only when a sequence
     * header starts a new decoder group. Giving every picture a PTS makes the
     * legacy MPEG parser repeatedly resynchronise and interlaced output stalls.
     * Keep Kodi's packet PTS for status, but mirror the sink's frame ABI. */
    size_t index;
    for (index = 0; index + 3U < prepared_size; ++index)
    {
      if (prepared[index] == 0x00U && prepared[index + 1U] == 0x00U &&
          prepared[index + 2U] == 0x01U && prepared[index + 3U] == 0xb3U)
      {
        frame.pts = (uint64_t)pts;
        break;
      }
    }
  }

  while (offset < prepared_size)
  {
    int poll_result;
    int written;
    uint64_t decoder_pts = 0;
    memset(descriptors, 0, sizeof(descriptors));
    descriptors[0].fd = instance->video_fd;
    descriptors[0].events = POLLOUT | POLLPRI;
    descriptors[1].fd = instance->poll_fd;
    descriptors[1].events = POLLIN;
    do
    {
      poll_result = poll(descriptors, 2, -1);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0 ||
        (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
      dream_log_errno(instance, STBP_LOG_ERROR, "polling Dream decoder");
      result = STBP_ERROR_IO;
      goto done;
    }
    if ((descriptors[0].revents & POLLPRI) != 0 ||
        (descriptors[1].revents & POLLIN) != 0)
    {
      memset(&event, 0, sizeof(event));
      (void)ioctl(instance->video_fd, VIDEO_GET_EVENT, &event);
    }
    if ((descriptors[0].revents & POLLOUT) == 0)
      continue;

    frame.bytes[0] = (ssize_t)(prepared_size - offset);
    frame.data[0] = prepared + offset;
    written = ioctl(instance->video_fd, DREAM_VIDEO_SET_FRAME, &frame);
    if (written < 0)
    {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      dream_log_errno(instance, STBP_LOG_ERROR, "VIDEO_SET_FRAME");
      result = STBP_ERROR_IO;
      goto done;
    }
    if (written == 0 || (size_t)written > prepared_size - offset)
    {
      dream_log(instance, STBP_LOG_ERROR,
                "Dream decoder returned an invalid VIDEO_SET_FRAME byte count");
      result = STBP_ERROR_IO;
      goto done;
    }
    offset += (size_t)written;
    (void)ioctl(instance->video_fd, VIDEO_GET_PTS, &decoder_pts);
    (void)ioctl(instance->video_fd, VIDEO_GET_PTS, &decoder_pts);
  }

  instance->send_codec_data = 0;
  instance->packets_queued++;
  if ((packet->flags & STBP_PACKET_DROP) != 0U)
    instance->packets_dropped++;
  if (pts != STBP_PTS_NONE)
    instance->last_pts_90k = pts;
  if (!instance->first_packet_logged)
  {
    char message[160];
    (void)snprintf(message, sizeof(message),
                   "first Dream access unit submitted: %zu bytes, pts=%lld",
                   prepared_size, (long long)pts);
    dream_log(instance, STBP_LOG_INFO, message);
    instance->first_packet_logged = 1;
  }
  result = STBP_OK;

done:
  free(prepared);
  if (result < 0)
    instance->last_error = result;
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result dream_get_buffer_state(void* opaque,
                                               struct stbp_buffer_state* state)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  enum stbp_result result = STBP_OK;
  if (instance == NULL || state == NULL || state->struct_size < sizeof(*state))
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state == STBP_STATE_CLOSED || instance->state == STBP_STATE_ERROR)
    result = STBP_ERROR_BAD_STATE;
  else
  {
    state->queued_bytes = 0;
    state->capacity_bytes = DREAM_QUEUE_BYTES;
    state->queued_packets = 0;
    /* queue_packet() performs the authoritative blocking POLLOUT wait.  Do not
     * expose the fresh decoder's transient non-writable state to Kodi here:
     * VC_NONE makes VideoPlayer accept priority messages only, which starves
     * the very first normal demux packet needed to select the decoder clock. */
    state->can_accept_packet = instance->state == STBP_STATE_OPEN;
  }
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result dream_get_status(void* opaque, struct stbp_status* status)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  uint64_t decoder_pts = 0;
  if (instance == NULL || status == NULL || status->struct_size < sizeof(*status))
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  status->state = instance->state;
  status->last_error = instance->last_error;
  status->presentation_pts_90k = instance->last_pts_90k;
  if (instance->video_fd >= 0 && ioctl(instance->video_fd, VIDEO_GET_PTS, &decoder_pts) == 0 &&
      (decoder_pts > 1U || instance->last_pts_90k <= 1))
    status->presentation_pts_90k = (int64_t)decoder_pts;
  status->packets_queued = instance->packets_queued;
  status->packets_dropped = instance->packets_dropped;
  (void)pthread_mutex_unlock(&instance->mutex);
  return STBP_OK;
}

static enum stbp_result dream_flush(void* opaque, int64_t next_pts_90k)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  enum stbp_result result;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state == STBP_STATE_CLOSED)
    result = STBP_ERROR_BAD_STATE;
  else
  {
    (void)ioctl(instance->video_fd, VIDEO_FREEZE, 0);
    (void)ioctl(instance->video_fd, VIDEO_STOP, 0);
    (void)ioctl(instance->video_fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX);
    result = start_decoder_locked(instance);
    if (result == STBP_OK)
    {
      instance->state = STBP_STATE_OPEN;
      instance->send_codec_data = instance->codec_data_size != 0U;
      instance->last_pts_90k = next_pts_90k;
      instance->first_packet_logged = 0;
    }
  }
  instance->last_error = result;
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result dream_drain(void* opaque)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  enum stbp_result result = STBP_OK;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN && instance->state != STBP_STATE_PAUSED &&
      instance->state != STBP_STATE_DRAINING)
    result = STBP_ERROR_BAD_STATE;
  else
    instance->state = STBP_STATE_DRAINING;
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result dream_reset(void* opaque)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  enum stbp_result result;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  result = close_locked(instance);
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result dream_set_speed(void* opaque, struct stbp_rational speed)
{
  (void)opaque;
  if (speed.denominator <= 0)
    return STBP_ERROR_INVALID_ARGUMENT;
  return speed.numerator == speed.denominator ? STBP_OK : STBP_ERROR_UNSUPPORTED;
}

static enum stbp_result dream_set_paused(void* opaque, int paused)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  enum stbp_result result = STBP_OK;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN && instance->state != STBP_STATE_PAUSED)
    result = STBP_ERROR_BAD_STATE;
  else if (ioctl(instance->video_fd, paused ? VIDEO_FREEZE : VIDEO_CONTINUE, 0) < 0)
  {
    dream_log_errno(instance, STBP_LOG_ERROR,
                    paused ? "VIDEO_FREEZE" : "VIDEO_CONTINUE");
    result = STBP_ERROR_IO;
  }
  else
    instance->state = paused ? STBP_STATE_PAUSED : STBP_STATE_OPEN;
  instance->last_error = result;
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result dream_set_video_rect(void* opaque,
                                             const struct stbp_rect* source,
                                             const struct stbp_rect* destination)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  char axis[96];
  int length;
  enum stbp_result result = STBP_OK;
  (void)source;
  if (instance == NULL || destination == NULL || destination->width <= 0 ||
      destination->height <= 0)
    return STBP_ERROR_INVALID_ARGUMENT;
  length = snprintf(axis, sizeof(axis), "%d %d %d %d", destination->x, destination->y,
                    destination->x + destination->width - 1,
                    destination->y + destination->height - 1);
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state == STBP_STATE_CLOSED)
    result = STBP_ERROR_BAD_STATE;
  else if (length <= 0 || (size_t)length >= sizeof(axis) ||
           write_text(DREAM_VIDEO_AXIS, axis) != 0 ||
           write_number(DREAM_VIDEO_SCREEN_MODE, 1) != 0)
  {
    dream_log_errno(instance, STBP_LOG_WARNING, "setting Dream video rectangle");
    result = STBP_ERROR_IO;
  }
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result dream_set_visible(void* opaque, int visible)
{
  struct dream_instance* instance = (struct dream_instance*)opaque;
  enum stbp_result result = STBP_OK;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state == STBP_STATE_CLOSED)
    result = STBP_ERROR_BAD_STATE;
  else if (instance->visible != (visible != 0) &&
           write_number(DREAM_DISABLE_VIDEO, visible ? 0 : 1) != 0)
  {
    dream_log_errno(instance, STBP_LOG_WARNING, "setting Dream video visibility");
    result = STBP_ERROR_IO;
  }
  else
    instance->visible = visible != 0;
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result dream_close(void* opaque)
{
  return dream_reset(opaque);
}

static const struct stbp_backend_api_v1 dream_api = {
    STBP_ABI_VERSION_1,
    sizeof(struct stbp_backend_api_v1),
    "dream-aml",
    "0.1.0-direct-amvideo",
    dream_create,
    dream_destroy,
    dream_probe,
    dream_open,
    dream_queue_packet,
    dream_get_buffer_state,
    dream_get_status,
    dream_flush,
    dream_drain,
    dream_reset,
    dream_set_speed,
    dream_set_paused,
    dream_set_video_rect,
    dream_set_visible,
    dream_close};

STBP_EXPORT const struct stbp_backend_api_v1* stbp_backend_get_api(uint32_t host_abi_version,
                                                                   uint32_t host_api_size)
{
  if (host_abi_version != STBP_ABI_VERSION_1 || host_api_size < sizeof(dream_api))
    return NULL;
  return &dream_api;
}
