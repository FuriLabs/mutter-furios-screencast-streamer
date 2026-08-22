/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef DRM_UTILS_H
#define DRM_UTILS_H

#include <glib.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

struct StreamState;
typedef struct StreamState StreamState;

typedef struct DrmBuffer
{
  uint32_t fb_id;
  uint32_t handle;
  uint32_t pitch;
  uint64_t size;
  void *map;

  uint32_t fb_w;
  uint32_t fb_h;
} DrmBuffer;

/**
 * DrmSink contains the DRM state used to display frames via dumb buffers.
 */
typedef struct DrmSink
{
  int card_index;              /* /dev/dri/cardN */
  char *connector_want;        /* e.g. "DVI-I-1" (optional) */

  int drm_fd;
  uint32_t conn_id;
  uint32_t crtc_id;
  drmModeModeInfo mode;
  int have_mode;

  DrmBuffer bufs[2];
  int front_idx;
  int pending_flip;
  int pending_flip_next_front;

  guint drm_source_id;

  uint32_t fb_w;
  uint32_t fb_h;

  gboolean atomic_ready;
  uint32_t plane_id;

  uint32_t plane_fb_id_prop;
  uint32_t plane_crtc_id_prop;

  uint32_t plane_src_x_prop;
  uint32_t plane_src_y_prop;
  uint32_t plane_src_w_prop;
  uint32_t plane_src_h_prop;

  uint32_t plane_crtc_x_prop;
  uint32_t plane_crtc_y_prop;
  uint32_t plane_crtc_w_prop;
  uint32_t plane_crtc_h_prop;

  uint32_t plane_in_fence_fd_prop;
} DrmSink;

/**
 * Release DRM resources associated with the sink.
 *
 * @param s DRM sink to clean up
 */
void
drm_cleanup(DrmSink *s);

/**
 * Ensure DRM device is opened, a connector/mode is selected,
 * dumb buffers are created, and the CRTC is set.
 *
 * @param st Stream state
 */
void
ensure_drm_ready(StreamState *st);

#endif // DRM_UTILS_H
