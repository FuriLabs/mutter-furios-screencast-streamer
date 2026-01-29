/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef STREAM_H
#define STREAM_H

#include <gio/gio.h>

#include "drm.h"

struct MetaFuriosMemfdHeader;
typedef struct MetaFuriosMemfdHeader MetaFuriosMemfdHeader;

typedef struct DamageRect
{
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
} DamageRect;

/**
 * StreamState holds the DBus session/stream objects, mapped memfd state,
 * and DRM sink state for displaying frames.
 */
typedef struct StreamState
{
  GDBusConnection *bus;

  char *session_path;
  char *stream_path;

  int memfd;
  void *map_base;
  size_t map_len;
  MetaFuriosMemfdHeader *hdr;

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

  DrmSink sink;
} StreamState;

#endif // STREAM_H
