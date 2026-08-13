/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HiSilicon AVPLAY backend for the first-generation OE-Alliance HiSi boxes.
 *
 * The filename and ABI name intentionally remain "hisi-dvb" for the first
 * migration step, so existing Kodi packages select this implementation
 * without a Kodi rebuild.  Unlike the former prototype this code does not
 * feed PES packets to /dev/dvb/adapter0/video0.  It creates a vendor AVPLAY
 * in ES mode and submits Kodi's compressed packets directly.  This is the
 * same hardware path used by the old HiPlayer patch, kept behind the stable
 * libstbplayer ABI instead of forking Kodi's VideoPlayer.
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
#include <unistd.h>

#define HI_SUCCESS 0
#define HI_FALSE 0
#define HI_TRUE 1

#define HI_UNF_DISPLAY1 1
#define HI_UNF_VO_DEV_MODE_NORMAL 0
#define HI_UNF_VO_ASPECT_CVRS_IGNORE 0

#define HI_UNF_AVPLAY_STREAM_TYPE_ES 1
#define HI_UNF_AVPLAY_BUF_ID_ES_VID 0
#define HI_UNF_AVPLAY_MEDIA_CHAN_VID 0x02
#define HI_UNF_AVPLAY_ATTR_ID_VDEC 2
#define HI_UNF_AVPLAY_ATTR_ID_SYNC 6
#define HI_UNF_SYNC_REF_NONE 0
#define HI_UNF_AVPLAY_STOP_MODE_BLACK 1
#define HI_UNF_VCODEC_MODE_NORMAL 0

#define HI_UNF_VCODEC_DEC_TYPE_NORMAL 0
/* The MV310 SDK ABI is newer than the enum copied by the old HiPlayer
 * patch.  Zgemma's matching libplayer.so opens its video channel with
 * { NORMAL, 12, 1 } for both Full-HD and 4096x2160 streams.  In this ABI
 * protocol level 1 is the normal H.264/MPEG-compatible pool layout. */
#define HI_UNF_VCODEC_CAP_LEVEL_MV310_UHD 12
#define HI_UNF_VCODEC_PRTCL_LEVEL_MV310_NORMAL 1

#define HI_VCODEC_MPEG2 0
#define HI_VCODEC_MPEG4 1
#define HI_VCODEC_H263 3
#define HI_VCODEC_H264 4
#define HI_VCODEC_VC1 7
#define HI_VCODEC_MJPEG 11
#define HI_VCODEC_VP8 16
#define HI_VCODEC_HEVC 36
#define HI_VCODEC_VP9 38

#define HISI_DVB_VIDEO_DEVICE "/dev/dvb/adapter0/video0"
#define HISI_DVB_STREAMTYPE_VP8 8
#define HISI_DVB_STREAMTYPE_VP9 9
#define HISI_FFMPEG_VDEC_LIBRARY "libHV.VIDEO.FFMPEG_VDEC.decode.so"
#define HISI_FFMPEG_AVCODEC_LIBRARY "libavcodec.so.57"
#define HISI_FFMPEG_AVCODEC_ID_VP8 140
#define HISI_FFMPEG_AVCODEC_ID_VP9 168
#define HISI_MAX_PACKET_SIZE (8U * 1024U * 1024U)
#define HISI_VIDEO_BUFFER_SIZE (20U * 1024U * 1024U)
#define HISI_AUDIO_BUFFER_SIZE (2U * 1024U * 1024U)
#define HISI_INVALID_PTS_MS UINT32_C(0xffffffff)

typedef int32_t hi_s32;
typedef uint32_t hi_u32;
typedef int32_t hi_bool;
typedef hi_u32 hi_handle;

#if STBP_HISI_MV310
typedef void (*ffmpeg57_register_all_fn)(void);
typedef const void* (*ffmpeg57_find_decoder_fn)(int codec_id);
typedef void* (*ffmpeg57_alloc_context_fn)(const void* codec);
typedef void (*ffmpeg57_free_context_fn)(void** context);
#endif

struct hi_rect
{
  hi_s32 x;
  hi_s32 y;
  hi_s32 width;
  hi_s32 height;
};

struct hi_crop_rect
{
  hi_u32 left;
  hi_u32 top;
  hi_u32 right;
  hi_u32 bottom;
};

struct hi_window_aspect
{
  hi_s32 conversion;
  hi_bool user_defined;
  hi_u32 width;
  hi_u32 height;
};

/* ABI layout of HI_UNF_WINDOW_ATTR_S from the 32-bit HiSilicon SDK. */
struct hi_window_attr
{
  hi_s32 display;
  hi_bool is_virtual;
  hi_s32 video_format;
  struct hi_window_aspect aspect;
  hi_bool use_crop_rect;
  struct hi_crop_rect crop;
  struct hi_rect input;
  struct hi_rect output;
};

struct hi_avplay_stream_attr
{
  hi_s32 stream_type;
  hi_u32 video_buffer_size;
  hi_u32 audio_buffer_size;
};

struct hi_avplay_attr
{
  hi_u32 demux_id;
  struct hi_avplay_stream_attr stream;
};

/* ABI layout of HI_UNF_AVPLAY_OPEN_OPT_S.  Passing this explicitly is
 * required by the MV310 AVPLAY implementation for VP8/VP9.  Its NULL
 * default allocates an H.264/Full-HD channel and SetAttr then rejects those
 * codecs even though the decoder hardware supports them. */
struct hi_avplay_open_options
{
  hi_s32 decoder_type;
  hi_s32 capacity_level;
  hi_s32 protocol_level;
};

struct hi_sync_region
{
  hi_s32 video_plus_time;
  hi_s32 video_negative_time;
  hi_bool smooth_play;
};

/* ABI layout of HI_UNF_SYNC_ATTR_S from the 32-bit HiSilicon SDK. */
struct hi_sync_attr
{
  hi_s32 reference;
  struct hi_sync_region start_region;
  struct hi_sync_region novel_region;
  hi_s32 video_pts_adjust;
  hi_s32 audio_pts_adjust;
  hi_u32 presync_timeout_ms;
  hi_bool quick_output;
};

union hi_vcodec_ext_attr
{
  struct
  {
    hi_bool advanced_profile;
    hi_u32 codec_version;
  } vc1;
  struct
  {
    hi_bool reversed;
    uint16_t display_width;
    uint16_t display_height;
  } vp6;
};

/* ABI layout of HI_UNF_VCODEC_ATTR_S on 32-bit ARM. */
struct hi_vcodec_attr
{
  hi_s32 type;
  union hi_vcodec_ext_attr extension;
  hi_s32 mode;
  hi_u32 error_cover;
  hi_u32 priority;
  hi_bool ordered_output;
  hi_s32 control_options;
  void* codec_context;
};

struct hi_stream_buffer
{
  uint8_t* data;
  hi_u32 size;
};

struct hi_stop_options
{
  hi_u32 timeout_ms;
  hi_s32 mode;
};

struct hi_flush_options
{
  hi_u32 reserved;
};

_Static_assert(sizeof(void*) == 4, "SF8008 HiSilicon ABI requires 32-bit pointers");
_Static_assert(sizeof(struct hi_window_attr) == 80, "unexpected HI_UNF_WINDOW_ATTR_S layout");
_Static_assert(sizeof(struct hi_avplay_attr) == 16, "unexpected HI_UNF_AVPLAY_ATTR_S layout");
_Static_assert(sizeof(struct hi_avplay_open_options) == 12,
               "unexpected HI_UNF_AVPLAY_OPEN_OPT_S layout");
_Static_assert(sizeof(struct hi_sync_attr) == 44, "unexpected HI_UNF_SYNC_ATTR_S layout");
_Static_assert(sizeof(struct hi_vcodec_attr) == 36, "unexpected HI_UNF_VCODEC_ATTR_S layout");
_Static_assert(sizeof(struct hi_stream_buffer) == 8, "unexpected HI_UNF_STREAM_BUF_S layout");

/* Exported by the receiver vendor's libhi_common.so and libhi_msp.so. */
extern hi_s32 HI_SYS_Init(void);
extern hi_s32 HI_SYS_DeInit(void);
extern hi_s32 HI_UNF_VO_Init(hi_s32 mode);
extern hi_s32 HI_UNF_VO_DeInit(void);
extern hi_s32 HI_UNF_VO_CreateWindow(const struct hi_window_attr* attr, hi_handle* window);
extern hi_s32 HI_UNF_VO_DestroyWindow(hi_handle window);
extern hi_s32 HI_UNF_VO_AttachWindow(hi_handle window, hi_handle source);
extern hi_s32 HI_UNF_VO_DetachWindow(hi_handle window, hi_handle source);
extern hi_s32 HI_UNF_VO_SetWindowEnable(hi_handle window, hi_bool enabled);
extern hi_s32 HI_UNF_VO_GetWindowAttr(hi_handle window, struct hi_window_attr* attr);
extern hi_s32 HI_UNF_VO_SetWindowAttr(hi_handle window, const struct hi_window_attr* attr);

extern hi_s32 HI_UNF_AVPLAY_Init(void);
extern hi_s32 HI_UNF_AVPLAY_DeInit(void);
#if STBP_HISI_MV310
extern hi_s32 HI_UNF_AVPLAY_RegisterVcodecLib(const char* filename);
#endif
extern hi_s32 HI_UNF_AVPLAY_GetDefaultConfig(struct hi_avplay_attr* attr, hi_s32 stream_type);
extern hi_s32 HI_UNF_AVPLAY_Create(const struct hi_avplay_attr* attr, hi_handle* avplay);
extern hi_s32 HI_UNF_AVPLAY_Destroy(hi_handle avplay);
/* Some HiSilicon SDK revisions expose an optional fourth output argument.
 * Dinobot's own U571 HiPlayer passes NULL here, and older three-argument ARM
 * implementations safely ignore the additional zero argument. */
extern hi_s32 HI_UNF_AVPLAY_ChnOpen(hi_handle avplay,
                                    hi_s32 channel,
                                    const void* options,
                                    hi_handle* channel_handle);
extern hi_s32 HI_UNF_AVPLAY_ChnClose(hi_handle avplay, hi_s32 channel);
extern hi_s32 HI_UNF_AVPLAY_GetAttr(hi_handle avplay, hi_s32 attribute, void* value);
extern hi_s32 HI_UNF_AVPLAY_SetAttr(hi_handle avplay, hi_s32 attribute, void* value);
extern hi_s32 HI_UNF_AVPLAY_Start(hi_handle avplay, hi_s32 channel, const void* options);
extern hi_s32 HI_UNF_AVPLAY_Stop(hi_handle avplay,
                                hi_s32 channel,
                                const struct hi_stop_options* options);
extern hi_s32 HI_UNF_AVPLAY_Pause(hi_handle avplay, const void* options);
extern hi_s32 HI_UNF_AVPLAY_Resume(hi_handle avplay, const void* options);
extern hi_s32 HI_UNF_AVPLAY_Reset(hi_handle avplay, const void* options);
extern hi_s32 HI_UNF_AVPLAY_GetBuf(hi_handle avplay,
                                  hi_s32 buffer_id,
                                  hi_u32 requested,
                                  struct hi_stream_buffer* buffer,
                                  hi_u32 timeout_ms);
extern hi_s32 HI_UNF_AVPLAY_PutBuf(hi_handle avplay,
                                   hi_s32 buffer_id,
                                   hi_u32 valid,
                                   hi_u32 pts_ms);
extern hi_s32 HI_UNF_AVPLAY_IsBuffEmpty(hi_handle avplay, hi_bool* empty);
extern hi_s32 HI_UNF_AVPLAY_FlushStream(hi_handle avplay,
                                       struct hi_flush_options* options);
extern hi_s32 HI_UNF_AVPLAY_GetStatusInfo(hi_handle avplay, void* status);

struct hisi_instance
{
  struct stbp_host_callbacks host;
  pthread_mutex_t mutex;
  enum stbp_state state;
  enum stbp_result last_error;
  enum stbp_codec codec;
  hi_handle avplay;
  hi_handle window;
  int sys_initialized;
  int vo_initialized;
  int avplay_initialized;
  int avplay_created;
  int window_created;
  int channel_open;
  int window_attached;
  int video_started;
  int visible;
  uint8_t* codec_data;
  size_t codec_data_size;
  int send_codec_data;
  int linux_dvb_mode;
  int video_fd;
  uint8_t* pending;
  size_t pending_size;
  size_t pending_offset;
  size_t pending_boundary;
  uint64_t dvb_write_calls;
  uint64_t dvb_bytes_written;
  uint64_t packets_queued;
  uint64_t packets_dropped;
  int64_t last_pts_90k;
  unsigned int get_buffer_failures;
#if STBP_HISI_MV310
  void* ffmpeg57_library;
  void* ffmpeg57_codec_context;
  ffmpeg57_free_context_fn ffmpeg57_free_context;
#endif
};

static void hisi_log(struct hisi_instance* instance,
                     enum stbp_log_level level,
                     const char* message)
{
  if (instance != NULL && instance->host.log != NULL)
    instance->host.log(instance->host.userdata, level, message);
}

static void hisi_log_result(struct hisi_instance* instance,
                            enum stbp_log_level level,
                            const char* operation,
                            hi_s32 result)
{
  char message[256];
  (void)snprintf(message, sizeof(message), "%s failed: 0x%08x", operation,
                 (unsigned int)result);
  hisi_log(instance, level, message);
}

#if STBP_HISI_MV310
static const char* mv310_ffmpeg_vdec_library(void)
{
  static const char* const paths[] = {
      "/usr/lib/hisilicon/" HISI_FFMPEG_VDEC_LIBRARY,
      "/usr/lib/" HISI_FFMPEG_VDEC_LIBRARY,
  };
  size_t index;

  for (index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index)
  {
    if (access(paths[index], R_OK) == 0)
      return paths[index];
  }
  return NULL;
}

static const char* mv310_ffmpeg_avcodec_library(void)
{
  static const char* const paths[] = {
      "/usr/lib/hisilicon/" HISI_FFMPEG_AVCODEC_LIBRARY,
      "/usr/lib/" HISI_FFMPEG_AVCODEC_LIBRARY,
  };
  size_t index;

  for (index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index)
  {
    if (access(paths[index], R_OK) == 0)
      return paths[index];
  }
  return NULL;
}

static int mv310_ffmpeg_codec_id(enum stbp_codec codec)
{
  if (codec == STBP_CODEC_VP8)
    return HISI_FFMPEG_AVCODEC_ID_VP8;
  if (codec == STBP_CODEC_VP9)
    return HISI_FFMPEG_AVCODEC_ID_VP9;
  return -1;
}

static int mv310_load_symbol(void* library, const char* name, void* output, size_t output_size)
{
  const char* error;
  void* symbol;

  if (output_size != sizeof(symbol))
    return 0;
  (void)dlerror();
  symbol = dlsym(library, name);
  error = dlerror();
  if (error != NULL || symbol == NULL)
    return 0;
  memcpy(output, &symbol, sizeof(symbol));
  return 1;
}

static void mv310_release_ffmpeg_context(struct hisi_instance* instance)
{
  if (instance->ffmpeg57_codec_context != NULL && instance->ffmpeg57_free_context != NULL)
    instance->ffmpeg57_free_context(&instance->ffmpeg57_codec_context);
  instance->ffmpeg57_codec_context = NULL;
  instance->ffmpeg57_free_context = NULL;
  if (instance->ffmpeg57_library != NULL)
    (void)dlclose(instance->ffmpeg57_library);
  instance->ffmpeg57_library = NULL;
}

static enum stbp_result mv310_create_ffmpeg_context(struct hisi_instance* instance,
                                                     enum stbp_codec codec)
{
  const char* library_path = mv310_ffmpeg_avcodec_library();
  ffmpeg57_register_all_fn register_all = NULL;
  ffmpeg57_find_decoder_fn find_decoder = NULL;
  ffmpeg57_alloc_context_fn alloc_context = NULL;
  const void* decoder;
  int codec_id = mv310_ffmpeg_codec_id(codec);

  if (library_path == NULL || codec_id < 0)
  {
    hisi_log(instance, STBP_LOG_ERROR,
             "MV310 vendor " HISI_FFMPEG_AVCODEC_LIBRARY " is missing");
    return STBP_ERROR_UNSUPPORTED;
  }
  instance->ffmpeg57_library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
  if (instance->ffmpeg57_library == NULL)
  {
    char message[320];
    const char* error = dlerror();
    (void)snprintf(message, sizeof(message), "dlopen(%s) failed: %s", library_path,
                   error != NULL ? error : "unknown error");
    hisi_log(instance, STBP_LOG_ERROR, message);
    return STBP_ERROR_BACKEND;
  }
  if (!mv310_load_symbol(instance->ffmpeg57_library, "avcodec_register_all", &register_all,
                         sizeof(register_all)) ||
      !mv310_load_symbol(instance->ffmpeg57_library, "avcodec_find_decoder", &find_decoder,
                         sizeof(find_decoder)) ||
      !mv310_load_symbol(instance->ffmpeg57_library, "avcodec_alloc_context3", &alloc_context,
                         sizeof(alloc_context)) ||
      !mv310_load_symbol(instance->ffmpeg57_library, "avcodec_free_context",
                         &instance->ffmpeg57_free_context,
                         sizeof(instance->ffmpeg57_free_context)))
  {
    hisi_log(instance, STBP_LOG_ERROR,
             "MV310 vendor FFmpeg-57 codec-context functions are incomplete");
    mv310_release_ffmpeg_context(instance);
    return STBP_ERROR_BACKEND;
  }

  register_all();
  decoder = find_decoder(codec_id);
  if (decoder == NULL)
  {
    hisi_log(instance, STBP_LOG_ERROR, "MV310 vendor FFmpeg decoder was not found");
    mv310_release_ffmpeg_context(instance);
    return STBP_ERROR_UNSUPPORTED;
  }
  instance->ffmpeg57_codec_context = alloc_context(decoder);
  if (instance->ffmpeg57_codec_context == NULL)
  {
    hisi_log(instance, STBP_LOG_ERROR, "MV310 vendor FFmpeg codec context allocation failed");
    mv310_release_ffmpeg_context(instance);
    return STBP_ERROR_BACKEND;
  }
  return STBP_OK;
}
#endif

static int codec_to_hisi(enum stbp_codec codec)
{
  switch (codec)
  {
    case STBP_CODEC_MPEG1:
    case STBP_CODEC_MPEG2:
      return HI_VCODEC_MPEG2;
    case STBP_CODEC_MPEG4_PART2:
      return HI_VCODEC_MPEG4;
    case STBP_CODEC_H263:
      return HI_VCODEC_H263;
    case STBP_CODEC_H264:
      return HI_VCODEC_H264;
    case STBP_CODEC_HEVC:
      return HI_VCODEC_HEVC;
    case STBP_CODEC_VC1:
      return HI_VCODEC_VC1;
    case STBP_CODEC_VP8:
      return HI_VCODEC_VP8;
    case STBP_CODEC_VP9:
      return HI_VCODEC_VP9;
    case STBP_CODEC_MJPEG:
      return HI_VCODEC_MJPEG;
    default:
      return -1;
  }
}

static hi_u32 packet_pts_ms(const struct stbp_packet* packet)
{
  int64_t pts = packet->pts_90k;
  if (pts == STBP_PTS_NONE)
    pts = packet->dts_90k;
  if (pts == STBP_PTS_NONE || pts < 0)
    return HISI_INVALID_PTS_MS;
  return (hi_u32)(((uint64_t)pts / UINT64_C(90)) & UINT32_MAX);
}

static void release_codec_data(struct hisi_instance* instance)
{
  free(instance->codec_data);
  instance->codec_data = NULL;
  instance->codec_data_size = 0;
  instance->send_codec_data = 0;
}

static void release_dvb_pending(struct hisi_instance* instance)
{
  free(instance->pending);
  instance->pending = NULL;
  instance->pending_size = 0;
  instance->pending_offset = 0;
  instance->pending_boundary = 0;
}

static void hisi_log_errno(struct hisi_instance* instance,
                           enum stbp_log_level level,
                           const char* operation)
{
  char message[256];
  const int saved_errno = errno;
  (void)snprintf(message, sizeof(message), "%s failed: %s (%d)", operation,
                 strerror(saved_errno), saved_errno);
  hisi_log(instance, level, message);
}

static size_t make_dvb_pes_header(uint8_t header[14], int64_t pts_90k)
{
  uint64_t pts;

  header[0] = 0x00;
  header[1] = 0x00;
  header[2] = 0x01;
  header[3] = 0xe0;
  header[4] = 0x00;
  header[5] = 0x00;
  header[6] = 0x81;
  if (pts_90k == STBP_PTS_NONE || pts_90k < 0)
  {
    header[7] = 0x00;
    header[8] = 0x00;
    return 9U;
  }

  pts = (uint64_t)pts_90k & UINT64_C(0x1ffffffff);
  header[7] = 0x80;
  header[8] = 0x05;
  header[9] = (uint8_t)(0x21U | ((pts >> 29) & 0x0eU));
  header[10] = (uint8_t)(pts >> 22);
  header[11] = (uint8_t)(0x01U | ((pts >> 14) & 0xfeU));
  header[12] = (uint8_t)(pts >> 7);
  header[13] = (uint8_t)(0x01U | ((pts << 1) & 0xfeU));
  return 14U;
}

static void set_dvb_pes_payload_size(uint8_t header[14], size_t payload_size)
{
  if (payload_size > UINT16_MAX)
    payload_size = 0;
  header[4] = (uint8_t)(payload_size >> 8);
  header[5] = (uint8_t)payload_size;
}

static size_t make_dvb_bcmv_header(uint8_t header[10],
                                   enum stbp_codec codec,
                                   size_t data_size)
{
  const uint32_t length = (uint32_t)(data_size + 10U);
  memcpy(header, "BCMV", 4U);
  header[4] = (uint8_t)(length >> 24);
  header[5] = (uint8_t)(length >> 16);
  header[6] = (uint8_t)(length >> 8);
  header[7] = (uint8_t)length;
  header[8] = 0U;
  header[9] = codec == STBP_CODEC_VP9 ? 1U : 0U;
  return 10U;
}

static enum stbp_result flush_dvb_pending_locked(struct hisi_instance* instance)
{
  while (instance->pending_offset < instance->pending_size)
  {
    size_t write_end = instance->pending_size;
    ssize_t written;

    /* HiSilicon's reference dvbvideosink writes PES/BCMV and the encoded
     * frame in separate syscalls. Preserve that boundary for MV310. */
    if (instance->pending_offset < instance->pending_boundary)
      write_end = instance->pending_boundary;
    written = write(instance->video_fd, instance->pending + instance->pending_offset,
                    write_end - instance->pending_offset);
    if (written > 0)
    {
      ++instance->dvb_write_calls;
      instance->dvb_bytes_written += (uint64_t)written;
      instance->pending_offset += (size_t)written;
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return STBP_AGAIN;
    hisi_log_errno(instance, STBP_LOG_ERROR, "writing HiSilicon DVB video PES");
    instance->last_error = STBP_ERROR_IO;
    return STBP_ERROR_IO;
  }
  release_dvb_pending(instance);
  return STBP_OK;
}

static enum stbp_result prepare_dvb_packet_locked(struct hisi_instance* instance,
                                                  const struct stbp_packet* packet)
{
  uint8_t pes_header[14];
  uint8_t bcmv_header[10];
  const int64_t pts = packet->pts_90k != STBP_PTS_NONE ? packet->pts_90k
                                                        : packet->dts_90k;
  const size_t pes_size = make_dvb_pes_header(pes_header, pts);
  const size_t bcmv_size = make_dvb_bcmv_header(bcmv_header, instance->codec,
                                                packet->size);
  size_t total_size;

  if (pes_size > SIZE_MAX - bcmv_size || pes_size + bcmv_size > SIZE_MAX - packet->size)
    return STBP_ERROR_INVALID_ARGUMENT;
  total_size = pes_size + bcmv_size + packet->size;
  set_dvb_pes_payload_size(pes_header, pes_size - 6U + bcmv_size + packet->size);
  instance->pending = (uint8_t*)malloc(total_size);
  if (instance->pending == NULL)
    return STBP_ERROR_BACKEND;
  memcpy(instance->pending, pes_header, pes_size);
  memcpy(instance->pending + pes_size, bcmv_header, bcmv_size);
  if (packet->size != 0U)
    memcpy(instance->pending + pes_size + bcmv_size, packet->data, packet->size);
  instance->pending_size = total_size;
  instance->pending_offset = 0U;
  instance->pending_boundary = pes_size + bcmv_size;
  return STBP_OK;
}

static int dvb_can_accept_packet(int fd)
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

static enum stbp_result open_linux_dvb_locked(struct hisi_instance* instance,
                                              const struct stbp_stream_config* stream)
{
  const int stream_type = stream->codec == STBP_CODEC_VP8 ? HISI_DVB_STREAMTYPE_VP8
                                                           : HISI_DVB_STREAMTYPE_VP9;
  hi_s32 result;
  char message[256];

  /* Enigma2 has already initialised the legacy HiSilicon system layer before
   * dvbvideosink opens /dev/dvb/adapter0/video0. Kodi starts after Enigma2
   * exits, so establish that process-local SDK state explicitly before using
   * the same kernel DVB path. */
  result = HI_SYS_Init();
  if (result != HI_SUCCESS)
  {
    hisi_log_result(instance, STBP_LOG_ERROR, "HI_SYS_Init(MV310 DVB)", result);
    return STBP_ERROR_BACKEND;
  }
  instance->sys_initialized = 1;

  instance->video_fd = open(HISI_DVB_VIDEO_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (instance->video_fd < 0)
  {
    hisi_log_errno(instance, STBP_LOG_ERROR, "opening " HISI_DVB_VIDEO_DEVICE);
    return errno == EBUSY ? STBP_ERROR_BUSY : STBP_ERROR_NO_DEVICE;
  }
  if (ioctl(instance->video_fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY) < 0)
  {
    hisi_log_errno(instance, STBP_LOG_ERROR, "VIDEO_SELECT_SOURCE(memory)");
    return STBP_ERROR_BACKEND;
  }
  if (ioctl(instance->video_fd, VIDEO_FREEZE) < 0)
    hisi_log_errno(instance, STBP_LOG_DEBUG, "initial VIDEO_FREEZE");
  if (ioctl(instance->video_fd, VIDEO_SET_STREAMTYPE, stream_type) < 0)
  {
    hisi_log_errno(instance, STBP_LOG_ERROR, "VIDEO_SET_STREAMTYPE");
    return STBP_ERROR_UNSUPPORTED;
  }
  if (ioctl(instance->video_fd, VIDEO_PLAY) < 0)
  {
    hisi_log_errno(instance, STBP_LOG_ERROR, "VIDEO_PLAY");
    return STBP_ERROR_BACKEND;
  }
  if (ioctl(instance->video_fd, VIDEO_CONTINUE) < 0)
  {
    hisi_log_errno(instance, STBP_LOG_ERROR, "VIDEO_CONTINUE");
    return STBP_ERROR_BACKEND;
  }
  instance->linux_dvb_mode = 1;
  instance->codec = stream->codec;
  instance->state = STBP_STATE_OPEN;
  instance->last_error = STBP_OK;
  instance->last_pts_90k = STBP_PTS_NONE;
  (void)snprintf(message, sizeof(message),
                 "HiSilicon MV310 Linux-DVB PES video path opened: codec=%d streamtype=%d %ux%u profile=%d",
                 (int)stream->codec, stream_type, stream->width, stream->height,
                 stream->codec_profile);
  hisi_log(instance, STBP_LOG_INFO, message);
  return STBP_OK;
}

static enum stbp_result queue_linux_dvb_locked(struct hisi_instance* instance,
                                               const struct stbp_packet* packet)
{
  enum stbp_result result = flush_dvb_pending_locked(instance);
  int64_t pts;

  if (result != STBP_OK)
    return result;
  if ((packet->flags & STBP_PACKET_END_OF_STREAM) != 0 && packet->size == 0U)
    return STBP_ERROR_UNSUPPORTED;
  if ((packet->flags & STBP_PACKET_DISCONTINUITY) != 0)
  {
    if (ioctl(instance->video_fd, VIDEO_CLEAR_BUFFER) < 0)
    {
      hisi_log_errno(instance, STBP_LOG_ERROR, "VIDEO_CLEAR_BUFFER(discontinuity)");
      return STBP_ERROR_IO;
    }
    (void)ioctl(instance->video_fd, VIDEO_CONTINUE);
  }
  result = prepare_dvb_packet_locked(instance, packet);
  if (result != STBP_OK)
    return result;
  if (instance->packets_queued < 4U)
  {
    char message[320];
    const uint8_t* data = packet->data;
    const unsigned int b0 = packet->size > 0U ? data[0] : 0U;
    const unsigned int b1 = packet->size > 1U ? data[1] : 0U;
    const unsigned int b2 = packet->size > 2U ? data[2] : 0U;
    const unsigned int b3 = packet->size > 3U ? data[3] : 0U;
    (void)snprintf(message, sizeof(message),
                   "MV310 DVB packet %llu: size=%zu pts=%lld dts=%lld flags=0x%x data=%02x%02x%02x%02x",
                   (unsigned long long)instance->packets_queued, packet->size,
                   (long long)packet->pts_90k, (long long)packet->dts_90k,
                   packet->flags, b0, b1, b2, b3);
    hisi_log(instance, STBP_LOG_INFO, message);
  }
  result = flush_dvb_pending_locked(instance);
  if (result == STBP_AGAIN)
    result = STBP_OK;
  if (result != STBP_OK)
    return result;

  ++instance->packets_queued;
  if (instance->packets_queued <= 4U)
  {
    char message[192];
    (void)snprintf(message, sizeof(message),
                   "MV310 DVB submitted: packets=%llu writes=%llu bytes=%llu pending=%zu",
                   (unsigned long long)instance->packets_queued,
                   (unsigned long long)instance->dvb_write_calls,
                   (unsigned long long)instance->dvb_bytes_written,
                   instance->pending_size - instance->pending_offset);
    hisi_log(instance, STBP_LOG_INFO, message);
  }
  if ((packet->flags & STBP_PACKET_DROP) != 0)
    ++instance->packets_dropped;
  pts = packet->pts_90k != STBP_PTS_NONE ? packet->pts_90k : packet->dts_90k;
  instance->last_pts_90k = pts;
  return STBP_OK;
}

static enum stbp_result close_locked(struct hisi_instance* instance)
{
  hi_s32 result;
  enum stbp_result final_result = STBP_OK;
  struct hi_stop_options stop = {0, HI_UNF_AVPLAY_STOP_MODE_BLACK};

  release_dvb_pending(instance);
  if (instance->video_fd >= 0)
  {
    if (ioctl(instance->video_fd, VIDEO_STOP) < 0 && errno != EINVAL)
    {
      hisi_log_errno(instance, STBP_LOG_WARNING, "VIDEO_STOP");
      final_result = STBP_ERROR_IO;
    }
    (void)ioctl(instance->video_fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX);
    (void)ioctl(instance->video_fd, VIDEO_CLEAR_BUFFER);
    (void)close(instance->video_fd);
    instance->video_fd = -1;
  }
  instance->linux_dvb_mode = 0;

  if (instance->state == STBP_STATE_PAUSED && instance->avplay_created)
    (void)HI_UNF_AVPLAY_Resume(instance->avplay, NULL);
  if (instance->video_started)
  {
    result = HI_UNF_AVPLAY_Stop(instance->avplay, HI_UNF_AVPLAY_MEDIA_CHAN_VID, &stop);
    if (result != HI_SUCCESS)
    {
      hisi_log_result(instance, STBP_LOG_WARNING, "HI_UNF_AVPLAY_Stop", result);
      final_result = STBP_ERROR_BACKEND;
    }
    instance->video_started = 0;
  }
  if (instance->window_created)
    (void)HI_UNF_VO_SetWindowEnable(instance->window, HI_FALSE);
  if (instance->window_attached)
  {
    (void)HI_UNF_VO_DetachWindow(instance->window, instance->avplay);
    instance->window_attached = 0;
  }
  if (instance->channel_open)
  {
    (void)HI_UNF_AVPLAY_ChnClose(instance->avplay, HI_UNF_AVPLAY_MEDIA_CHAN_VID);
    instance->channel_open = 0;
  }
  if (instance->window_created)
  {
    (void)HI_UNF_VO_DestroyWindow(instance->window);
    instance->window = 0;
    instance->window_created = 0;
  }
  if (instance->avplay_created)
  {
    (void)HI_UNF_AVPLAY_Destroy(instance->avplay);
    instance->avplay = 0;
    instance->avplay_created = 0;
  }
  if (instance->avplay_initialized)
  {
    (void)HI_UNF_AVPLAY_DeInit();
    instance->avplay_initialized = 0;
  }
#if STBP_HISI_MV310
  mv310_release_ffmpeg_context(instance);
#endif
  if (instance->vo_initialized)
  {
    (void)HI_UNF_VO_DeInit();
    instance->vo_initialized = 0;
  }
  if (instance->sys_initialized)
  {
    (void)HI_SYS_DeInit();
    instance->sys_initialized = 0;
  }

  release_codec_data(instance);
  instance->state = STBP_STATE_CLOSED;
  instance->codec = STBP_CODEC_UNKNOWN;
  instance->visible = 0;
  instance->last_pts_90k = STBP_PTS_NONE;
  instance->get_buffer_failures = 0;
  return final_result;
}

static enum stbp_result hisi_create(const struct stbp_host_callbacks* host, void** output)
{
  struct hisi_instance* instance;
  if (host == NULL || host->struct_size < sizeof(*host) || output == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  instance = (struct hisi_instance*)calloc(1, sizeof(*instance));
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

static void hisi_destroy(void* opaque)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  if (instance == NULL)
    return;
  (void)pthread_mutex_lock(&instance->mutex);
  (void)close_locked(instance);
  (void)pthread_mutex_unlock(&instance->mutex);
  (void)pthread_mutex_destroy(&instance->mutex);
  free(instance);
}

static enum stbp_result hisi_probe(void* opaque, struct stbp_capabilities* capabilities)
{
  (void)opaque;
  if (capabilities == NULL || capabilities->struct_size < sizeof(*capabilities))
    return STBP_ERROR_INVALID_ARGUMENT;
  /* HiSilicon SDK packaging differs between receiver families.  Octagon and
   * several Dinobot images install the entry libraries directly in
   * /usr/lib, while the GFutures HD60/HD61 driver package keeps the complete
   * legacy SDK in /usr/lib/hisilicon.  Both layouts expose the same AVPLAY
   * ABI and device nodes, so do not reject the latter before the backend can
   * even be selected. */
  if (access("/usr/lib/libhi_msp.so", R_OK) != 0 &&
      access("/usr/lib/hisilicon/libhi_msp.so", R_OK) != 0)
    return STBP_ERROR_NO_DEVICE;
  if (access("/dev/hi_vdec", R_OK | W_OK) != 0 ||
      access("/dev/hi_vo", R_OK | W_OK) != 0)
    return STBP_ERROR_NO_DEVICE;

  capabilities->codec_mask = STBP_CODEC_BIT(STBP_CODEC_MPEG1) |
                             STBP_CODEC_BIT(STBP_CODEC_MPEG2) |
                             STBP_CODEC_BIT(STBP_CODEC_MPEG4_PART2) |
                             STBP_CODEC_BIT(STBP_CODEC_H263) |
                             STBP_CODEC_BIT(STBP_CODEC_H264) |
                             STBP_CODEC_BIT(STBP_CODEC_HEVC) |
                             STBP_CODEC_BIT(STBP_CODEC_VC1) |
                             STBP_CODEC_BIT(STBP_CODEC_VP8) |
                             STBP_CODEC_BIT(STBP_CODEC_VP9) |
                             STBP_CODEC_BIT(STBP_CODEC_MJPEG);
  capabilities->feature_mask = STBP_FEATURE_PAUSE | STBP_FEATURE_VIDEO_RECT |
                               STBP_FEATURE_VISIBILITY | STBP_FEATURE_PRESENTATION_CLOCK |
                               STBP_FEATURE_DRAIN | STBP_FEATURE_INTERLACED;
  capabilities->max_width = 3840;
  capabilities->max_height = 2160;
  capabilities->max_packet_size = HISI_MAX_PACKET_SIZE;
  capabilities->preferred_queue_bytes = HISI_VIDEO_BUFFER_SIZE;
  return STBP_OK;
}

static enum stbp_result hisi_open(void* opaque,
                                  const struct stbp_stream_config* stream,
                                  const struct stbp_clock_config* clock)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  struct hi_avplay_attr avplay_attr;
  struct hi_sync_attr sync_attr;
  struct hi_window_attr window_attr;
  struct hi_vcodec_attr codec_attr;
#if STBP_HISI_MV310
  struct hi_avplay_open_options open_options;
  const char* external_vcodec_library = NULL;
#endif
  const void* channel_options = NULL;
  hi_s32 result;
  int codec;
  enum stbp_result final_result = STBP_ERROR_BACKEND;
  (void)clock;

  if (instance == NULL || stream == NULL || stream->struct_size < sizeof(*stream) ||
      (clock != NULL && clock->struct_size < sizeof(*clock)) ||
      (stream->extra_data == NULL && stream->extra_data_size != 0))
    return STBP_ERROR_INVALID_ARGUMENT;
  if ((stream->flags & STBP_STREAM_ENCRYPTED) != 0 ||
      ((stream->codec == STBP_CODEC_H264 || stream->codec == STBP_CODEC_HEVC) &&
       (stream->flags & STBP_STREAM_ANNEX_B) == 0))
    return STBP_ERROR_UNSUPPORTED;
  codec = codec_to_hisi(stream->codec);
  if (codec < 0 || stream->extra_data_size > HISI_MAX_PACKET_SIZE)
    return STBP_ERROR_UNSUPPORTED;

  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_CLOSED)
  {
    final_result = STBP_ERROR_BAD_STATE;
    goto done;
  }
  if (stream->extra_data_size != 0)
  {
    instance->codec_data = (uint8_t*)malloc(stream->extra_data_size);
    if (instance->codec_data == NULL)
      goto failed;
    memcpy(instance->codec_data, stream->extra_data, stream->extra_data_size);
    instance->codec_data_size = stream->extra_data_size;
    instance->send_codec_data = 1;
  }

#define HISI_OPEN_CALL(call, name)                                                                 \
  do                                                                                               \
  {                                                                                                \
    result = (call);                                                                                \
    if (result != HI_SUCCESS)                                                                       \
    {                                                                                              \
      hisi_log_result(instance, STBP_LOG_ERROR, (name), result);                                   \
      goto failed;                                                                                 \
    }                                                                                              \
  } while (0)

  HISI_OPEN_CALL(HI_SYS_Init(), "HI_SYS_Init");
  instance->sys_initialized = 1;
  HISI_OPEN_CALL(HI_UNF_VO_Init(HI_UNF_VO_DEV_MODE_NORMAL), "HI_UNF_VO_Init");
  instance->vo_initialized = 1;
  HISI_OPEN_CALL(HI_UNF_AVPLAY_Init(), "HI_UNF_AVPLAY_Init");
  instance->avplay_initialized = 1;

#if STBP_HISI_MV310
  /* MV310's built-in VDEC table does not create VP8/VP9 codecs itself.  The
   * receiver player registers this vendor FFmpeg adapter, whose capability
   * table maps both HiSilicon codec IDs (VP8=29 and VP9=30).  Registration is
   * process-local and duplicate registration is explicitly accepted by the
   * SDK, so perform it after every AVPLAY init before creating the channel. */
  if (stream->codec == STBP_CODEC_VP8 || stream->codec == STBP_CODEC_VP9)
  {
    external_vcodec_library = mv310_ffmpeg_vdec_library();
    if (external_vcodec_library == NULL)
    {
      hisi_log(instance, STBP_LOG_ERROR,
               "MV310 VP8/VP9 decoder library " HISI_FFMPEG_VDEC_LIBRARY " is missing");
      final_result = STBP_ERROR_UNSUPPORTED;
      goto failed;
    }
    HISI_OPEN_CALL(HI_UNF_AVPLAY_RegisterVcodecLib(external_vcodec_library),
                   "HI_UNF_AVPLAY_RegisterVcodecLib(FFmpeg VDEC)");
    final_result = mv310_create_ffmpeg_context(instance, stream->codec);
    if (final_result != STBP_OK)
      goto failed;
  }
#endif

  memset(&avplay_attr, 0, sizeof(avplay_attr));
  HISI_OPEN_CALL(HI_UNF_AVPLAY_GetDefaultConfig(&avplay_attr, HI_UNF_AVPLAY_STREAM_TYPE_ES),
                 "HI_UNF_AVPLAY_GetDefaultConfig");
  avplay_attr.demux_id = 0;
  avplay_attr.stream.video_buffer_size = HISI_VIDEO_BUFFER_SIZE;
  avplay_attr.stream.audio_buffer_size = HISI_AUDIO_BUFFER_SIZE;
  HISI_OPEN_CALL(HI_UNF_AVPLAY_Create(&avplay_attr, &instance->avplay),
                 "HI_UNF_AVPLAY_Create");
  instance->avplay_created = 1;

  /* Kodi owns audio separately through ALSA, so AVPLAY must not wait for an
   * audio or PCR clock that does not exist inside this video-only instance. */
  memset(&sync_attr, 0, sizeof(sync_attr));
  HISI_OPEN_CALL(HI_UNF_AVPLAY_GetAttr(instance->avplay, HI_UNF_AVPLAY_ATTR_ID_SYNC,
                                       &sync_attr),
                 "HI_UNF_AVPLAY_GetAttr(SYNC)");
  sync_attr.reference = HI_UNF_SYNC_REF_NONE;
  HISI_OPEN_CALL(HI_UNF_AVPLAY_SetAttr(instance->avplay, HI_UNF_AVPLAY_ATTR_ID_SYNC,
                                       &sync_attr),
                 "HI_UNF_AVPLAY_SetAttr(SYNC)");

  memset(&window_attr, 0, sizeof(window_attr));
  window_attr.display = HI_UNF_DISPLAY1;
  window_attr.is_virtual = HI_FALSE;
  window_attr.aspect.conversion = HI_UNF_VO_ASPECT_CVRS_IGNORE;
  HISI_OPEN_CALL(HI_UNF_VO_CreateWindow(&window_attr, &instance->window),
                 "HI_UNF_VO_CreateWindow");
  instance->window_created = 1;

#if STBP_HISI_MV310
  if (stream->codec == STBP_CODEC_VP8 || stream->codec == STBP_CODEC_VP9)
  {
    memset(&open_options, 0, sizeof(open_options));
    open_options.decoder_type = HI_UNF_VCODEC_DEC_TYPE_NORMAL;
    open_options.capacity_level = HI_UNF_VCODEC_CAP_LEVEL_MV310_UHD;
    open_options.protocol_level = HI_UNF_VCODEC_PRTCL_LEVEL_MV310_NORMAL;
    channel_options = &open_options;
  }
#endif

  HISI_OPEN_CALL(HI_UNF_AVPLAY_ChnOpen(instance->avplay, HI_UNF_AVPLAY_MEDIA_CHAN_VID,
                                       channel_options, NULL),
                 "HI_UNF_AVPLAY_ChnOpen(video)");
  instance->channel_open = 1;

  memset(&codec_attr, 0, sizeof(codec_attr));
  HISI_OPEN_CALL(HI_UNF_AVPLAY_GetAttr(instance->avplay, HI_UNF_AVPLAY_ATTR_ID_VDEC,
                                       &codec_attr),
                 "HI_UNF_AVPLAY_GetAttr(VDEC)");
  codec_attr.type = codec;
  codec_attr.mode = HI_UNF_VCODEC_MODE_NORMAL;
  codec_attr.error_cover = 100;
  codec_attr.priority = 3;
#if STBP_HISI_MV310
  if (stream->codec == STBP_CODEC_VP8 || stream->codec == STBP_CODEC_VP9)
    codec_attr.codec_context = instance->ffmpeg57_codec_context;
#endif
  if (stream->codec == STBP_CODEC_VC1)
  {
    codec_attr.extension.vc1.advanced_profile = stream->codec_profile == 3 ? HI_TRUE : HI_FALSE;
    codec_attr.extension.vc1.codec_version = 8;
  }
  HISI_OPEN_CALL(HI_UNF_AVPLAY_SetAttr(instance->avplay, HI_UNF_AVPLAY_ATTR_ID_VDEC,
                                       &codec_attr),
                 "HI_UNF_AVPLAY_SetAttr(VDEC)");

  HISI_OPEN_CALL(HI_UNF_VO_AttachWindow(instance->window, instance->avplay),
                 "HI_UNF_VO_AttachWindow");
  instance->window_attached = 1;
  HISI_OPEN_CALL(HI_UNF_VO_SetWindowEnable(instance->window, HI_TRUE),
                 "HI_UNF_VO_SetWindowEnable");
  instance->visible = 1;
  HISI_OPEN_CALL(HI_UNF_AVPLAY_Start(instance->avplay, HI_UNF_AVPLAY_MEDIA_CHAN_VID, NULL),
                 "HI_UNF_AVPLAY_Start(video)");
  instance->video_started = 1;

#undef HISI_OPEN_CALL

  instance->codec = stream->codec;
  instance->state = STBP_STATE_OPEN;
  instance->last_error = STBP_OK;
  instance->last_pts_90k = STBP_PTS_NONE;
  hisi_log(instance, STBP_LOG_INFO, "HiSilicon AVPLAY ES video path opened");
  final_result = STBP_OK;
  goto done;

failed:
  (void)close_locked(instance);
done:
  (void)pthread_mutex_unlock(&instance->mutex);
  return final_result;
}

static enum stbp_result hisi_queue_packet(void* opaque, const struct stbp_packet* packet)
{
  static const uint8_t vc1_frame_start[] = {0x00, 0x00, 0x01, 0x0d};
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  struct hi_stream_buffer buffer;
  const uint8_t* frame_header = NULL;
  size_t frame_header_size = 0;
  size_t total_size;
  size_t offset = 0;
  hi_s32 result;
  enum stbp_result final_result = STBP_OK;

  if (instance == NULL || packet == NULL || packet->struct_size < sizeof(*packet) ||
      (packet->data == NULL && packet->size != 0) || packet->size > HISI_MAX_PACKET_SIZE)
    return STBP_ERROR_INVALID_ARGUMENT;

  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN && instance->state != STBP_STATE_PAUSED)
  {
    final_result = STBP_ERROR_BAD_STATE;
    goto done;
  }
  if (instance->linux_dvb_mode)
  {
    final_result = queue_linux_dvb_locked(instance, packet);
    goto done;
  }
  if ((packet->flags & STBP_PACKET_END_OF_STREAM) != 0 && packet->size == 0)
  {
    final_result = STBP_ERROR_UNSUPPORTED;
    goto done;
  }
  if ((packet->flags & STBP_PACKET_DISCONTINUITY) != 0)
  {
    result = HI_UNF_AVPLAY_Reset(instance->avplay, NULL);
    if (result != HI_SUCCESS)
    {
      hisi_log_result(instance, STBP_LOG_ERROR, "HI_UNF_AVPLAY_Reset", result);
      final_result = STBP_ERROR_BACKEND;
      goto done;
    }
    instance->send_codec_data = instance->codec_data_size != 0;
  }
  if (instance->codec == STBP_CODEC_VC1)
  {
    frame_header = vc1_frame_start;
    frame_header_size = sizeof(vc1_frame_start);
  }

  /* Dinobot's original HiPlayer submits Annex-B codec data as its own ES
   * buffer at PTS zero before the first access unit.  In particular, U571's
   * stream parser does not reliably recognize a small opening IDR when the
   * SPS/PPS block is concatenated with that access unit. */
  if (instance->send_codec_data && instance->codec_data_size != 0)
  {
    memset(&buffer, 0, sizeof(buffer));
    result = HI_UNF_AVPLAY_GetBuf(instance->avplay, HI_UNF_AVPLAY_BUF_ID_ES_VID,
                                  (hi_u32)instance->codec_data_size, &buffer, 0);
    if (result != HI_SUCCESS)
    {
      ++instance->get_buffer_failures;
      if (instance->get_buffer_failures == 1 || instance->get_buffer_failures % 256 == 0)
        hisi_log_result(instance, STBP_LOG_DEBUG, "HI_UNF_AVPLAY_GetBuf(codec data)", result);
      final_result = STBP_AGAIN;
      goto done;
    }
    instance->get_buffer_failures = 0;
    if (buffer.data == NULL || buffer.size < instance->codec_data_size)
    {
      hisi_log(instance, STBP_LOG_ERROR,
               "HI_UNF_AVPLAY_GetBuf returned a short codec-data buffer");
      final_result = STBP_ERROR_BACKEND;
      goto done;
    }
    memcpy(buffer.data, instance->codec_data, instance->codec_data_size);
    result = HI_UNF_AVPLAY_PutBuf(instance->avplay, HI_UNF_AVPLAY_BUF_ID_ES_VID,
                                  (hi_u32)instance->codec_data_size, 0);
    if (result != HI_SUCCESS)
    {
      hisi_log_result(instance, STBP_LOG_ERROR, "HI_UNF_AVPLAY_PutBuf(codec data)", result);
      instance->last_error = STBP_ERROR_BACKEND;
      final_result = STBP_ERROR_BACKEND;
      goto done;
    }
    instance->send_codec_data = 0;
  }

  if (frame_header_size > SIZE_MAX - packet->size)
  {
    final_result = STBP_ERROR_INVALID_ARGUMENT;
    goto done;
  }
  total_size = frame_header_size + packet->size;
  if (total_size > HISI_MAX_PACKET_SIZE || total_size > UINT32_MAX)
  {
    final_result = STBP_ERROR_INVALID_ARGUMENT;
    goto done;
  }

  memset(&buffer, 0, sizeof(buffer));
  result = HI_UNF_AVPLAY_GetBuf(instance->avplay, HI_UNF_AVPLAY_BUF_ID_ES_VID,
                                (hi_u32)total_size, &buffer, 0);
  if (result != HI_SUCCESS)
  {
    ++instance->get_buffer_failures;
    if (instance->get_buffer_failures == 1 || instance->get_buffer_failures % 256 == 0)
      hisi_log_result(instance, STBP_LOG_DEBUG, "HI_UNF_AVPLAY_GetBuf(video)", result);
    final_result = STBP_AGAIN;
    goto done;
  }
  instance->get_buffer_failures = 0;
  if (buffer.data == NULL || buffer.size < total_size)
  {
    hisi_log(instance, STBP_LOG_ERROR, "HI_UNF_AVPLAY_GetBuf returned a short buffer");
    final_result = STBP_ERROR_BACKEND;
    goto done;
  }
  if (frame_header_size != 0)
  {
    memcpy(buffer.data + offset, frame_header, frame_header_size);
    offset += frame_header_size;
  }
  if (packet->size != 0)
  {
    memcpy(buffer.data + offset, packet->data, packet->size);
  }

  result = HI_UNF_AVPLAY_PutBuf(instance->avplay, HI_UNF_AVPLAY_BUF_ID_ES_VID,
                                (hi_u32)total_size, packet_pts_ms(packet));
  if (result != HI_SUCCESS)
  {
    hisi_log_result(instance, STBP_LOG_ERROR, "HI_UNF_AVPLAY_PutBuf(video)", result);
    instance->last_error = STBP_ERROR_BACKEND;
    final_result = STBP_ERROR_BACKEND;
    goto done;
  }
  ++instance->packets_queued;
  if ((packet->flags & STBP_PACKET_DROP) != 0)
    ++instance->packets_dropped;
  if (packet->pts_90k != STBP_PTS_NONE)
    instance->last_pts_90k = packet->pts_90k;
  else if (packet->dts_90k != STBP_PTS_NONE)
    instance->last_pts_90k = packet->dts_90k;

done:
  (void)pthread_mutex_unlock(&instance->mutex);
  return final_result;
}

static enum stbp_result hisi_get_buffer_state(void* opaque, struct stbp_buffer_state* state)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  hi_bool empty = HI_FALSE;
  hi_s32 result;
  enum stbp_result final_result = STBP_OK;
  if (instance == NULL || state == NULL || state->struct_size < sizeof(*state))
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state == STBP_STATE_CLOSED || instance->state == STBP_STATE_ERROR)
  {
    final_result = STBP_ERROR_BAD_STATE;
    goto done;
  }
  if (instance->linux_dvb_mode)
  {
    if (instance->pending != NULL)
    {
      final_result = flush_dvb_pending_locked(instance);
      if (final_result == STBP_AGAIN)
        final_result = STBP_OK;
      else if (final_result != STBP_OK)
        goto done;
    }
    state->queued_bytes = (uint32_t)(instance->pending_size - instance->pending_offset);
    state->capacity_bytes = HISI_VIDEO_BUFFER_SIZE;
    state->queued_packets = instance->pending != NULL ? 1U : 0U;
    state->can_accept_packet = instance->state == STBP_STATE_OPEN &&
                               instance->pending == NULL &&
                               dvb_can_accept_packet(instance->video_fd);
    goto done;
  }
  result = HI_UNF_AVPLAY_IsBuffEmpty(instance->avplay, &empty);
  if (result != HI_SUCCESS)
    empty = HI_FALSE;
  state->queued_bytes = empty ? 0U : 1U;
  state->capacity_bytes = HISI_VIDEO_BUFFER_SIZE;
  state->queued_packets = empty ? 0U : 1U;
  state->can_accept_packet = instance->state == STBP_STATE_OPEN ||
                             instance->state == STBP_STATE_PAUSED;
done:
  (void)pthread_mutex_unlock(&instance->mutex);
  return final_result;
}

static enum stbp_result hisi_get_status(void* opaque, struct stbp_status* status)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  /* Large aligned storage; u32LastVidPts is word 3 of HI_UNF_AVPLAY_STATUS_INFO_S. */
  hi_u32 vendor_status[256];
  if (instance == NULL || status == NULL || status->struct_size < sizeof(*status))
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  status->state = instance->state;
  status->last_error = instance->last_error;
  status->presentation_pts_90k = instance->last_pts_90k;
  if (instance->linux_dvb_mode)
  {
    uint64_t decoder_pts = 0;
    if (ioctl(instance->video_fd, VIDEO_GET_PTS, &decoder_pts) == 0 &&
        (decoder_pts != 0U || instance->last_pts_90k == 0))
      status->presentation_pts_90k = (int64_t)decoder_pts;
    status->packets_queued = instance->packets_queued;
    status->packets_dropped = instance->packets_dropped;
    (void)pthread_mutex_unlock(&instance->mutex);
    return STBP_OK;
  }
  memset(vendor_status, 0, sizeof(vendor_status));
  if (instance->avplay_created &&
      HI_UNF_AVPLAY_GetStatusInfo(instance->avplay, vendor_status) == HI_SUCCESS &&
      vendor_status[3] != HISI_INVALID_PTS_MS)
    status->presentation_pts_90k = (int64_t)vendor_status[3] * INT64_C(90);
  status->packets_queued = instance->packets_queued;
  status->packets_dropped = instance->packets_dropped;
  (void)pthread_mutex_unlock(&instance->mutex);
  return STBP_OK;
}

static enum stbp_result hisi_flush(void* opaque, int64_t next_pts_90k)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  hi_s32 result;
  enum stbp_result final_result = STBP_OK;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state == STBP_STATE_CLOSED)
    final_result = STBP_ERROR_BAD_STATE;
  else if (instance->linux_dvb_mode)
  {
    release_dvb_pending(instance);
    if (ioctl(instance->video_fd, VIDEO_CLEAR_BUFFER) < 0)
    {
      hisi_log_errno(instance, STBP_LOG_ERROR, "VIDEO_CLEAR_BUFFER");
      final_result = STBP_ERROR_IO;
    }
    else
    {
      (void)ioctl(instance->video_fd, VIDEO_CONTINUE);
      instance->state = STBP_STATE_OPEN;
      instance->last_pts_90k = next_pts_90k;
    }
  }
  else
  {
    result = HI_UNF_AVPLAY_Reset(instance->avplay, NULL);
    if (result != HI_SUCCESS)
    {
      hisi_log_result(instance, STBP_LOG_ERROR, "HI_UNF_AVPLAY_Reset", result);
      final_result = STBP_ERROR_BACKEND;
    }
    else
    {
      instance->state = STBP_STATE_OPEN;
      instance->send_codec_data = instance->codec_data_size != 0;
      instance->last_pts_90k = next_pts_90k;
    }
  }
  (void)pthread_mutex_unlock(&instance->mutex);
  return final_result;
}

static enum stbp_result hisi_drain(void* opaque)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  struct hi_flush_options options = {0};
  hi_s32 result;
  enum stbp_result final_result = STBP_OK;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN && instance->state != STBP_STATE_PAUSED)
    final_result = instance->state == STBP_STATE_DRAINING ? STBP_OK : STBP_ERROR_BAD_STATE;
  else if (instance->linux_dvb_mode)
  {
    final_result = flush_dvb_pending_locked(instance);
    if (final_result == STBP_OK)
      instance->state = STBP_STATE_DRAINING;
  }
  else
  {
    result = HI_UNF_AVPLAY_FlushStream(instance->avplay, &options);
    if (result != HI_SUCCESS)
    {
      hisi_log_result(instance, STBP_LOG_ERROR, "HI_UNF_AVPLAY_FlushStream", result);
      final_result = STBP_ERROR_BACKEND;
    }
    else
      instance->state = STBP_STATE_DRAINING;
  }
  (void)pthread_mutex_unlock(&instance->mutex);
  return final_result;
}

static enum stbp_result hisi_reset(void* opaque)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  enum stbp_result result;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  result = close_locked(instance);
  (void)pthread_mutex_unlock(&instance->mutex);
  return result;
}

static enum stbp_result hisi_set_speed(void* opaque, struct stbp_rational speed)
{
  (void)opaque;
  if (speed.denominator <= 0)
    return STBP_ERROR_INVALID_ARGUMENT;
  return speed.numerator == speed.denominator ? STBP_OK : STBP_ERROR_UNSUPPORTED;
}

static enum stbp_result hisi_set_paused(void* opaque, int paused)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  hi_s32 result;
  enum stbp_result final_result = STBP_OK;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->state != STBP_STATE_OPEN && instance->state != STBP_STATE_PAUSED)
    final_result = STBP_ERROR_BAD_STATE;
  else if (instance->linux_dvb_mode)
  {
    if (ioctl(instance->video_fd, paused ? VIDEO_FREEZE : VIDEO_CONTINUE) < 0)
    {
      hisi_log_errno(instance, STBP_LOG_ERROR,
                     paused ? "VIDEO_FREEZE" : "VIDEO_CONTINUE");
      final_result = STBP_ERROR_IO;
    }
    else
      instance->state = paused ? STBP_STATE_PAUSED : STBP_STATE_OPEN;
  }
  else
  {
    result = paused ? HI_UNF_AVPLAY_Pause(instance->avplay, NULL)
                    : HI_UNF_AVPLAY_Resume(instance->avplay, NULL);
    if (result != HI_SUCCESS)
    {
      hisi_log_result(instance, STBP_LOG_ERROR,
                      paused ? "HI_UNF_AVPLAY_Pause" : "HI_UNF_AVPLAY_Resume", result);
      final_result = STBP_ERROR_BACKEND;
    }
    else
      instance->state = paused ? STBP_STATE_PAUSED : STBP_STATE_OPEN;
  }
  (void)pthread_mutex_unlock(&instance->mutex);
  return final_result;
}

static enum stbp_result hisi_set_video_rect(void* opaque,
                                            const struct stbp_rect* source,
                                            const struct stbp_rect* destination)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  struct hi_window_attr attr;
  hi_s32 result;
  enum stbp_result final_result = STBP_OK;
  (void)source;
  if (instance == NULL || destination == NULL || destination->width < 0 ||
      destination->height < 0)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->linux_dvb_mode)
    final_result = STBP_ERROR_UNSUPPORTED;
  else if (!instance->window_created)
    final_result = STBP_ERROR_BAD_STATE;
  else
  {
    memset(&attr, 0, sizeof(attr));
    result = HI_UNF_VO_GetWindowAttr(instance->window, &attr);
    if (result == HI_SUCCESS)
    {
      attr.output.x = destination->x;
      attr.output.y = destination->y;
      attr.output.width = destination->width;
      attr.output.height = destination->height;
      result = HI_UNF_VO_SetWindowAttr(instance->window, &attr);
    }
    if (result != HI_SUCCESS)
    {
      hisi_log_result(instance, STBP_LOG_WARNING, "setting HiSilicon video rectangle", result);
      final_result = STBP_ERROR_BACKEND;
    }
  }
  (void)pthread_mutex_unlock(&instance->mutex);
  return final_result;
}

static enum stbp_result hisi_set_visible(void* opaque, int visible)
{
  struct hisi_instance* instance = (struct hisi_instance*)opaque;
  hi_s32 result;
  enum stbp_result final_result = STBP_OK;
  if (instance == NULL)
    return STBP_ERROR_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&instance->mutex);
  if (instance->linux_dvb_mode)
    final_result = STBP_ERROR_UNSUPPORTED;
  else if (!instance->window_created)
    final_result = STBP_ERROR_BAD_STATE;
  else if (instance->visible != (visible != 0))
  {
    result = HI_UNF_VO_SetWindowEnable(instance->window, visible ? HI_TRUE : HI_FALSE);
    if (result != HI_SUCCESS)
    {
      hisi_log_result(instance, STBP_LOG_WARNING, "HI_UNF_VO_SetWindowEnable", result);
      final_result = STBP_ERROR_BACKEND;
    }
    else
      instance->visible = visible != 0;
  }
  (void)pthread_mutex_unlock(&instance->mutex);
  return final_result;
}

static enum stbp_result hisi_close(void* opaque)
{
  return hisi_reset(opaque);
}

static const struct stbp_backend_api_v1 hisi_api = {
    STBP_ABI_VERSION_1,
    sizeof(struct stbp_backend_api_v1),
    "hisi-dvb",
    "0.3.4-avplay-mv310-ffmpeg-context",
    hisi_create,
    hisi_destroy,
    hisi_probe,
    hisi_open,
    hisi_queue_packet,
    hisi_get_buffer_state,
    hisi_get_status,
    hisi_flush,
    hisi_drain,
    hisi_reset,
    hisi_set_speed,
    hisi_set_paused,
    hisi_set_video_rect,
    hisi_set_visible,
    hisi_close};

STBP_EXPORT const struct stbp_backend_api_v1* stbp_backend_get_api(uint32_t host_abi_version,
                                                                   uint32_t host_api_size)
{
  if (host_abi_version != STBP_ABI_VERSION_1 || host_api_size < sizeof(hisi_api))
    return NULL;
  return &hisi_api;
}
