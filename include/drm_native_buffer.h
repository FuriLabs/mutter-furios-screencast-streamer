/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef DRM_NATIVE_BUFFER_H
#define DRM_NATIVE_BUFFER_H

struct StreamState;
typedef struct StreamState StreamState;

/**
 * Release imported FBs and GEM handles for the native-buffer backend.
 *
 * @param st Stream state
 */
void
drm_native_buffer_cleanup(StreamState *st);

/**
 * Render/present a native-buffer frame.
 *
 * @param st Stream state
 */
void
render_frame_drm_native_buffer(StreamState *st);

#endif // DRM_NATIVE_BUFFER_H
