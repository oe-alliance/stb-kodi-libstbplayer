/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hisi_audio_clock.h"

#include <errno.h>
#include <linux/dvb/video.h>
#include <pthread.h>
#include <sys/ioctl.h>

static pthread_mutex_t clock_mutex = PTHREAD_MUTEX_INITIALIZER;
static int clock_fd = -1;

void stbp_hisi_audio_clock_attach(int video_fd)
{
  if (video_fd < 0)
    return;
  (void)pthread_mutex_lock(&clock_mutex);
  clock_fd = video_fd;
  (void)pthread_mutex_unlock(&clock_mutex);
}

void stbp_hisi_audio_clock_detach(int video_fd)
{
  (void)pthread_mutex_lock(&clock_mutex);
  if (clock_fd == video_fd)
    clock_fd = -1;
  (void)pthread_mutex_unlock(&clock_mutex);
}

int64_t stbp_hisi_audio_clock_read(void)
{
  const int saved_errno = errno;
  uint64_t pts = 0;
  int64_t result = STBP_PTS_NONE;
  (void)pthread_mutex_lock(&clock_mutex);
  if (clock_fd >= 0 && ioctl(clock_fd, VIDEO_GET_PTS, &pts) == 0 &&
      pts > 0 && pts <= UINT64_C(0x1ffffffff))
    result = (int64_t)pts;
  (void)pthread_mutex_unlock(&clock_mutex);
  errno = saved_errno;
  return result;
}
