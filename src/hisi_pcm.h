/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef STBP_HISI_PCM_H
#define STBP_HISI_PCM_H
#include "stbplayer/audio.h"

struct stbp_hisi_pcm;
void stbp_hisi_pcm_select(const void* device, int enabled);
void stbp_hisi_pcm_unselect(const void* device);
/* Private library/backend interface; compressed audio returns UNSUPPORTED. */
STBP_EXPORT enum stbp_result stbp_hisi_pcm_flush_video(int video_fd);
enum stbp_result stbp_hisi_pcm_open(const struct stbp_host_callbacks* host,
    const struct stbp_audio_config* config, struct stbp_hisi_pcm** output);
void stbp_hisi_pcm_close(struct stbp_hisi_pcm* pcm);
enum stbp_result stbp_hisi_pcm_queue(struct stbp_hisi_pcm* pcm,
    const struct stbp_audio_packet* packet);
enum stbp_result stbp_hisi_pcm_pts(struct stbp_hisi_pcm* pcm, int64_t* pts);
enum stbp_result stbp_hisi_pcm_flush(struct stbp_hisi_pcm* pcm);
enum stbp_result stbp_hisi_pcm_drain(struct stbp_hisi_pcm* pcm);
enum stbp_result stbp_hisi_pcm_pause(struct stbp_hisi_pcm* pcm, int paused);
enum stbp_result stbp_hisi_pcm_volume(struct stbp_hisi_pcm* pcm, uint32_t left, uint32_t right);
#endif
