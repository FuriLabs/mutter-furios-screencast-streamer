/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef STREAM_H
#define STREAM_H

#include <gio/gio.h>

#include "drm_utils.h"
#include "memfd.h"
#include "utils.h"

typedef enum StreamBackendType
{
  STREAM_BACKEND_UNKNOWN = 0,
  STREAM_BACKEND_MEMFD = 1,
  STREAM_BACKEND_NATIVE_BUFFER = 2,
} StreamBackendType;

typedef struct NativeSlotImport
{
  int prime_fd_index;
  uint32_t gem_handle;
  uint32_t fb_id;
} NativeSlotImport;

struct StreamState;
typedef struct StreamState StreamState;

/**
 * Callback invoked from DRM page-flip handler when a render was deferred
 * due to an in-flight flip.
 *
 * @param st Stream state
 */
typedef void (*StreamRenderPendingFunc)(StreamState *st);

/**
 * Callback invoked when vblank timestamp is updated, used to re-arm
 * RequestFrame pacing for the memfd backend.
 *
 * @param st Stream state
 */
typedef void (*StreamVblankFunc)(StreamState *st);

struct StreamState
{
  GDBusConnection *bus;

  char *session_path;
  char *stream_path;

  StreamBackendType backend;
  StreamBackendType backend_override;

  guint32 info_width;
  guint32 info_height;
  double info_fps;

  int memfd;
  void *map_base;
  size_t map_len;
  MetaFuriosMemfdHeader *hdr;

  GVariant *native_info;
  int *native_fds;
  int native_n_fds;

  guint32 native_width;
  guint32 native_height;
  guint32 native_stride_pixels;
  guint32 native_n_slots;

  NativeSlotImport *native_slots;

  gboolean native_modeset_done;

  guint signal_sub_id;
  guint signal_sub_damage_id;

  guint request_timer_id;
  int request_timer_fd;
  guint request_timer_source_id;

  guint name_watch_id;
  guint delayed_start_id;

  gboolean service_present;
  gboolean streaming;
  gboolean request_in_flight;
  gboolean request_again;

  guint32 last_seen_seq;
  guint32 last_presented_seq;
  guint32 inflight_flip_seq;

  gboolean force_full_damage;

  gboolean need_render_after_flip;

  guint32 pending_seq;
  guint32 pending_slot;
  GArray *pending_damage;

  guint8 *cpu_buf;
  size_t cpu_buf_len;

  gboolean vblank_valid;
  guint64 vblank_last_ns;
  guint64 vblank_period_ns;
  guint64 vblank_lead_ns;

  StreamRenderPendingFunc render_pending_cb;
  StreamVblankFunc vblank_cb;

  DrmSink sink;
};

#endif // STREAM_H
