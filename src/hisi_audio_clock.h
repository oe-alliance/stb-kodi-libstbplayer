/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef STBP_HISI_AUDIO_CLOCK_H
#define STBP_HISI_AUDIO_CLOCK_H

#include "stbplayer/backend.h"

/* Internal backend/library interface. The backend retains ownership and must
 * detach before stopping or closing the descriptor. Detach waits for readers. */
STBP_EXPORT void stbp_hisi_audio_clock_attach(int video_fd);
STBP_EXPORT void stbp_hisi_audio_clock_detach(int video_fd);
int64_t stbp_hisi_audio_clock_read(void);

#endif
