/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef DRM_MEMFD_H
#define DRM_MEMFD_H

struct StreamState;
typedef struct StreamState StreamState;

/**
 * Render the latest MEMFD-backed frame to DRM and schedule a page flip.
 *
 * @param st Stream state
 */
void
drm_memfd_render_frame(StreamState *st);

#endif // DRM_MEMFD_H
