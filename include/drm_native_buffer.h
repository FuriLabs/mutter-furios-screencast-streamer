/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef DRM_NATIVE_BUFFER_H
#define DRM_NATIVE_BUFFER_H

#include <glib.h>

struct StreamState;
typedef struct StreamState StreamState;

/**
 * Initialize native-buffer metadata and allocate per-slot state.
 *
 * @param st Stream state
 * @return TRUE on success, FALSE otherwise
 */
gboolean
drm_native_buffer_init(StreamState *st);

/**
 * Import all native-buffer slots and create their DRM framebuffers.
 *
 * DRM must already be initialized before calling this function.
 *
 * @param st Stream state
 * @return TRUE on success, FALSE otherwise
 */
gboolean
drm_native_buffer_import_all(StreamState *st);

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
drm_native_buffer_render_frame(StreamState *st);

#endif // DRM_NATIVE_BUFFER_H
