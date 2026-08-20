/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "dbus.h"

#include <glib-unix.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>

#include "drm_memfd.h"
#include "drm_native_buffer.h"
#include "memfd.h"
#include "stream.h"
#include "utils.h"

static gboolean
dbus_variant_lookup_string(GVariant    *dict,
                           const char  *key,
                           char       **out_str)
{
  GVariant *v;

  if (!dict || !key || !out_str)
    return FALSE;

  v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_STRING);
  if (!v)
    return FALSE;

  *out_str = g_variant_dup_string(v, NULL);

  g_variant_unref(v);

  return *out_str != NULL;
}

static gboolean
dbus_variant_lookup_u32(GVariant   *dict,
                        const char *key,
                        guint32    *out_u32)
{
  if (!dict || !key || !out_u32)
    return FALSE;

  GVariant *v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_UINT32);
  if (!v)
    return FALSE;

  *out_u32 = g_variant_get_uint32(v);
  g_variant_unref(v);
  return TRUE;
}

static const char *
backend_name(StreamBackendType backend)
{
  switch (backend) {
  case STREAM_BACKEND_MEMFD:
    return "memfd";
  case STREAM_BACKEND_NATIVE_BUFFER:
    return "native-buffer";
  case STREAM_BACKEND_UNKNOWN:
  default:
    return "unknown";
  }
}

static gboolean
sequence_after(guint32 a,
               guint32 b)
{
  return (gint32)(a - b) > 0;
}

static gboolean
sequence_skipped(guint32 current,
                 guint32 previous)
{
  if (previous == 0)
    return FALSE;

  return current - previous > 1;
}

static GVariant *
call_sync(GDBusConnection *bus,
          const char     *dest,
          const char     *path,
          const char     *iface,
          const char     *method,
          GVariant       *params)
{
  g_autoptr(GError) error = NULL;

  GVariant *ret = g_dbus_connection_call_sync(bus,
                                              dest,
                                              path,
                                              iface,
                                              method,
                                              params,
                                              NULL,
                                              G_DBUS_CALL_FLAGS_NONE,
                                              -1,
                                              NULL,
                                              &error);
  if (!ret) {
    g_warning("DBus call %s failed: %s",
              method,
              error ? error->message : "unknown error");
    return NULL;
  }

  return ret;
}

static gboolean
get_info_dbus(GDBusConnection  *bus,
              const char       *stream_path,
              StreamState      *st,
              GVariant        **out_info)
{
  if (!bus || !stream_path || !st || !out_info)
    return FALSE;

  g_print("[GetInfo] calling GetInfo on %s\n", stream_path);

  g_autoptr(GVariant) ret = call_sync(bus,
                                     IFACE_SC,
                                     stream_path,
                                     IFACE_STREAM,
                                     "GetInfo",
                                     NULL);
  if (!ret)
    return FALSE;

  GVariant *dict = NULL;

  g_variant_get(ret, "(@a{sv})", &dict);
  if (!dict)
    return FALSE;

  *out_info = g_variant_ref(dict);
  g_variant_unref(dict);

  g_print("[GetInfo] got info dict for %s\n", stream_path);

  return TRUE;
}

static StreamBackendType
backend_from_info(GVariant    *info,
                  StreamState *st)
{
  st->backend = STREAM_BACKEND_UNKNOWN;
  st->info_width = 0;
  st->info_height = 0;
  st->info_fps = 0.0;

  g_autofree char *type_str = NULL;

  if (info) {
    if (dbus_variant_lookup_string(info, "type", &type_str) && type_str) {
      if (strcmp(type_str, "memfd") == 0)
        st->backend = STREAM_BACKEND_MEMFD;
      else if (strcmp(type_str, "native-buffer") == 0)
        st->backend = STREAM_BACKEND_NATIVE_BUFFER;
    }

    dbus_variant_lookup_u32(info, "width", &st->info_width);
    dbus_variant_lookup_u32(info, "height", &st->info_height);

    GVariant *v_fps_d = g_variant_lookup_value(info, "fps", G_VARIANT_TYPE_DOUBLE);
    if (v_fps_d) {
      st->info_fps = g_variant_get_double(v_fps_d);
      g_variant_unref(v_fps_d);
    } else {
      GVariant *v_fps_u = g_variant_lookup_value(info, "fps", G_VARIANT_TYPE_UINT32);
      if (v_fps_u) {
        st->info_fps = (double)g_variant_get_uint32(v_fps_u);
        g_variant_unref(v_fps_u);
      }
    }
  }

  g_print("[GetInfo] backend=%s width=%u height=%u fps=%f\n",
          backend_name(st->backend),
          st->info_width,
          st->info_height,
          st->info_fps);

  return st->backend;
}

static int
get_memfd_dbus(GDBusConnection *bus,
               const char      *stream_path)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GUnixFDList) out_fds = NULL;

  g_print("[GetMemfd] calling GetMemfd on %s\n", stream_path);

  g_autoptr(GVariant) ret = g_dbus_connection_call_with_unix_fd_list_sync(bus,
                                                                          IFACE_SC,
                                                                          stream_path,
                                                                          IFACE_STREAM,
                                                                          "GetMemfd",
                                                                          NULL,
                                                                          NULL,
                                                                          G_DBUS_CALL_FLAGS_NONE,
                                                                          2000,
                                                                          NULL,
                                                                          &out_fds,
                                                                          NULL,
                                                                          &err);
  if (!ret) {
    g_warning("GetMemfd failed: %s", err ? err->message : "unknown error");
    return -1;
  }

  gint handle_index = -1;

  g_variant_get(ret, "(h)", &handle_index);

  if (!out_fds || handle_index < 0) {
    g_warning("[GetMemfd] invalid fd list or handle index");
    return -1;
  }

  int fd = g_unix_fd_list_get(out_fds, handle_index, &err);
  if (fd < 0) {
    g_warning("g_unix_fd_list_get failed: %s",
              err ? err->message : "unknown error");
    return -1;
  }

  struct stat stbuf;

  if (fstat(fd, &stbuf) != 0) {
    g_warning("[GetMemfd] fstat failed: %s", g_strerror(errno));
    close(fd);
    return -1;
  }

  g_print("[GetMemfd] got fd=%d size=%zu\n", fd, (size_t)stbuf.st_size);

  return fd;
}

static gboolean
setup_memfd_backend(StreamState *st)
{
  if (!st || !st->bus || !st->stream_path)
    return FALSE;

  st->memfd.fd = get_memfd_dbus(st->bus, st->stream_path);
  if (st->memfd.fd < 0)
    return FALSE;

  struct stat stbuf;

  if (fstat(st->memfd.fd, &stbuf) != 0) {
    g_warning("fstat(memfd) failed: %s", g_strerror(errno));
    return FALSE;
  }

  st->memfd.map_len = (size_t)stbuf.st_size;
  st->memfd.map_base = mmap(NULL,
                            st->memfd.map_len,
                            PROT_READ,
                            MAP_SHARED,
                            st->memfd.fd,
                            0);
  if (st->memfd.map_base == MAP_FAILED) {
    g_warning("mmap(memfd) failed: %s", g_strerror(errno));
    return FALSE;
  }

  st->memfd.hdr = (MetaFuriosMemfdHeader *)st->memfd.map_base;

  if (!memfd_header_sane(st->memfd.hdr)) {
    g_warning("invalid memfd header");
    return FALSE;
  }

  st->last_seen_seq = st->memfd.hdr->seq;
  st->last_presented_seq = st->memfd.hdr->seq;
  st->inflight_flip_seq = 0;
  st->force_full_damage = TRUE;

  st->pending_seq = st->last_seen_seq;
  st->pending_slot = st->memfd.hdr->last_slot;

  return TRUE;
}

static gboolean
setup_native_buffer_backend(StreamState *st)
{
  if (!st || !st->bus || !st->stream_path)
    return FALSE;

  g_autoptr(GError) err = NULL;
  g_autoptr(GUnixFDList) out_fds = NULL;

  g_print("[GetNativeBufferHandle] calling on %s\n", st->stream_path);

  g_autoptr(GVariant) ret = g_dbus_connection_call_with_unix_fd_list_sync(st->bus,
                                                                          IFACE_SC,
                                                                          st->stream_path,
                                                                          IFACE_STREAM,
                                                                          "GetNativeBufferHandle",
                                                                          NULL,
                                                                          NULL,
                                                                          G_DBUS_CALL_FLAGS_NONE,
                                                                          5000,
                                                                          NULL,
                                                                          &out_fds,
                                                                          NULL,
                                                                          &err);
  if (!ret) {
    g_warning("GetNativeBufferHandle failed: %s", err ? err->message : "unknown error");
    return FALSE;
  }

  g_print("[GetNativeBufferHandle] ok\n");

  GVariant *dict = NULL;

  g_variant_get(ret, "(@a{sv})", &dict);
  if (!dict) {
    g_warning("GetNativeBufferHandle returned no dict");
    return FALSE;
  }

  if (st->native.info)
    g_variant_unref(st->native.info);

  st->native.info = g_variant_ref(dict);
  g_variant_unref(dict);

  int n = out_fds ? g_unix_fd_list_get_length(out_fds) : 0;

  st->native.n_fds = 0;
  g_clear_pointer(&st->native.fds, g_free);

  if (n > 0) {
    st->native.fds = g_new0(int, n);

    for (int i = 0; i < n; i++) {
      int fd = g_unix_fd_list_get(out_fds, i, &err);
      if (fd < 0) {
        g_warning("g_unix_fd_list_get(%d) failed: %s", i, err ? err->message : "unknown");
        st->native.fds[i] = -1;
        g_clear_error(&err);
        continue;
      }

      st->native.fds[i] = fd;
    }

    st->native.n_fds = n;
  }

  st->native.slots = NULL;
  st->native.n_slots = 0;
  st->native.stride_pixels = 0;
  st->native.width = 0;
  st->native.height = 0;
  st->native.modeset_done = FALSE;

  st->last_seen_seq = 0;
  st->last_presented_seq = 0;
  st->inflight_flip_seq = 0;
  st->force_full_damage = TRUE;

  st->pending_seq = 0;
  st->pending_slot = 0;

  if (!drm_native_buffer_init(st)) {
    g_warning("[native-buffer] failed to initialize native buffer metadata");
    return FALSE;
  }

  return TRUE;
}

static void
pending_damage_set_full(StreamState *st)
{
  if (!st)
    return;

  if (!st->pending_damage)
    st->pending_damage = g_array_new(FALSE, FALSE, sizeof(DamageRect));
  else
    g_array_set_size(st->pending_damage, 0);

  guint32 w = 0;
  guint32 h = 0;

  if (st->backend == STREAM_BACKEND_MEMFD && st->memfd.hdr) {
    w = st->memfd.hdr->width;
    h = st->memfd.hdr->height;
  } else {
    w = st->info_width;
    h = st->info_height;
  }

  if (w == 0)
    w = 1920;
  if (h == 0)
    h = 1080;

  DamageRect r;

  r.x = 0;
  r.y = 0;
  r.w = (int32_t)w;
  r.h = (int32_t)h;

  g_array_append_val(st->pending_damage, r);
}

static void
pending_damage_replace(StreamState *st,
                       GVariant    *damage)
{
  if (!st)
    return;

  if (!st->pending_damage)
    st->pending_damage = g_array_new(FALSE, FALSE, sizeof(DamageRect));
  else
    g_array_set_size(st->pending_damage, 0);

  if (!damage) {
    pending_damage_set_full(st);
    return;
  }

  GVariantIter iter;
  gint32 x = 0;
  gint32 y = 0;
  gint32 w = 0;
  gint32 h = 0;

  g_variant_iter_init(&iter, damage);

  while (g_variant_iter_next(&iter, "(iiii)", &x, &y, &w, &h)) {
    DamageRect r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;

    g_array_append_val(st->pending_damage, r);
  }

  if (st->pending_damage->len == 0)
    pending_damage_set_full(st);
}

static void
render_or_defer(StreamState *st)
{
  if (!st)
    return;

  if (st->sink.pending_flip) {
    st->need_render_after_flip = TRUE;
    return;
  }

  if (st->backend == STREAM_BACKEND_NATIVE_BUFFER)
    drm_native_buffer_render_frame(st);
  else
    drm_memfd_render_frame(st);
}

static void
on_frame_ready_common(StreamState *st,
                      guint32      seq,
                      guint32      slot,
                      GVariant    *damage_maybe)
{
  if (!st)
    return;

  if (!sequence_after(seq, st->last_seen_seq))
    return;

    if (st->sink.pending_flip ||
        sequence_skipped(seq, st->last_presented_seq))
      st->force_full_damage = TRUE;

  st->pending_seq = seq;
  st->pending_slot = slot;

  if (st->force_full_damage) {
    pending_damage_set_full(st);
  } else {
    if (damage_maybe)
      pending_damage_replace(st, damage_maybe);
    else
      pending_damage_set_full(st);
  }

  st->last_seen_seq = seq;

  render_or_defer(st);
}

static void
on_frame_ready(GDBusConnection *connection,
               const gchar     *sender_name,
               const gchar     *object_path,
               const gchar     *interface_name,
               const gchar     *signal_name,
               GVariant        *parameters,
               gpointer         user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;

  StreamState *st = user_data;
  guint32 seq = 0;
  guint32 slot = 0;

  g_variant_get(parameters, "(uu)", &seq, &slot);
  g_debug("[signal] %s(seq=%u slot=%u)",
          signal_name ? signal_name : "FrameReady",
          seq, slot);

  on_frame_ready_common(st, seq, slot, NULL);
}

static void
on_frame_ready_with_damage(GDBusConnection *connection,
                           const gchar     *sender_name,
                           const gchar     *object_path,
                           const gchar     *interface_name,
                           const gchar     *signal_name,
                           GVariant        *parameters,
                           gpointer         user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;

  StreamState *st = user_data;
  guint32 seq = 0;
  guint32 slot = 0;
  GVariant *damage = NULL;

  g_variant_get(parameters, "(uu@a(iiii))", &seq, &slot, &damage);
  g_debug("[signal] %s(seq=%u slot=%u)",
          signal_name ? signal_name : "FrameReadyWithDamage",
          seq, slot);

  on_frame_ready_common(st, seq, slot, damage);

  if (damage)
    g_variant_unref(damage);
}

void
stream_rearm_request_timer(StreamState *st)
{
  if (!st)
    return;

  if (st->backend == STREAM_BACKEND_NATIVE_BUFFER)
    return;

  if (st->request_timer_fd < 0)
    return;

  guint64 now = monotonic_ns();
  if (now == 0)
    return;

  guint64 period = st->vblank_period_ns;
  if (period == 0)
    period = 16666666ull;

  guint64 lead = st->vblank_lead_ns;

  lead = clamp_u64(lead, 0, period / 2);

  guint64 target = 0;

  if (st->vblank_valid && st->vblank_last_ns != 0) {
    guint64 last = st->vblank_last_ns;
    guint64 n = 1;

    if (now > last) {
      guint64 delta = now - last;
      n = (delta / period) + 1;
    }

    guint64 next_vblank = last + n * period;

    if (next_vblank > lead)
      target = next_vblank - lead;
    else
      target = now;
  } else {
    target = now + (guint64)REQUEST_INTERVAL_MS * 1000000ull;
  }

  if (target < now + 200000ull)
    target = now + 200000ull;

  arm_timerfd_abs_ns(st->request_timer_fd, target);
}

static void
on_request_frame_done(GObject      *source_object,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  (void)source_object;

  StreamState *st = user_data;
  if (!st)
    return;

  st->request_in_flight = FALSE;

  g_autoptr(GError) err = NULL;
  GVariant *ret = g_dbus_connection_call_finish(st->bus, res, &err);

  if (!ret) {
    g_warning("RequestFrame async error: %s",
              err ? err->message : "unknown error");

    st->request_again = FALSE;
    return;
  }

  g_debug("[RequestFrame] done ok");

  g_variant_unref(ret);

  /*
   * native-buffer backend may have asked for another frame when the
   * previous RequestFrame call was still completing
   */
  if (st->backend == STREAM_BACKEND_NATIVE_BUFFER &&
      st->streaming &&
      st->request_again) {
    request_next_frame(st);
    return;
  }

  if (st->request_timer_fd >= 0 &&
      st->backend != STREAM_BACKEND_NATIVE_BUFFER)
    stream_rearm_request_timer(st);
}

void
request_next_frame(StreamState *st)
{
  if (!st || !st->bus || !st->stream_path)
    return;

  if (!st->streaming)
    return;

  if (st->request_in_flight) {
    st->request_again = TRUE;
    return;
  }

  st->request_in_flight = TRUE;
  st->request_again = FALSE;

  g_debug("[RequestFrame] calling on %s", st->stream_path);

  g_dbus_connection_call(st->bus,
                         IFACE_SC,
                         st->stream_path,
                         IFACE_STREAM,
                         "RequestFrame",
                         NULL,
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         NULL,
                         on_request_frame_done,
                         st);
}

static gboolean
request_timerfd_cb(gint         fd,
                   GIOCondition cond,
                   gpointer     user_data)
{
  (void)cond;

  StreamState *st = user_data;
  if (!st)
    return G_SOURCE_CONTINUE;

  if (st->backend == STREAM_BACKEND_NATIVE_BUFFER)
    return G_SOURCE_CONTINUE;

  uint64_t expirations = 0;
  ssize_t n = read(fd, &expirations, sizeof(expirations));

  if (n < 0) {
    if (errno != EAGAIN)
      g_warning("timerfd read failed: %s", g_strerror(errno));
  }

  request_next_frame(st);

  if (st->request_in_flight)
    return G_SOURCE_CONTINUE;

  stream_rearm_request_timer(st);
  return G_SOURCE_CONTINUE;
}

static gboolean
request_frame_tick(gpointer data)
{
  StreamState *st = data;

  if (!st || !st->bus || !st->stream_path)
    return G_SOURCE_CONTINUE;
  if (!st->streaming)
    return G_SOURCE_CONTINUE;

  if (st->backend == STREAM_BACKEND_NATIVE_BUFFER)
    return G_SOURCE_CONTINUE;

  request_next_frame(st);
  return G_SOURCE_CONTINUE;
}

static void
start_streaming(StreamState *st)
{
  if (!st || st->streaming)
    return;

  st->streaming = TRUE;
  st->request_in_flight = FALSE;
  st->request_again = FALSE;

  if (st->backend == STREAM_BACKEND_NATIVE_BUFFER) {
    request_next_frame(st);
    return;
  }

  if (st->request_timer_fd < 0) {
    st->request_timer_fd = timerfd_create(CLOCK_MONOTONIC,
                                          TFD_NONBLOCK | TFD_CLOEXEC);

    if (st->request_timer_fd >= 0) {
      if (!st->request_timer_source_id)
        st->request_timer_source_id = g_unix_fd_add_full(G_PRIORITY_HIGH,
                                                         st->request_timer_fd,
                                                         (GIOCondition)(G_IO_IN |
                                                                        G_IO_HUP |
                                                                        G_IO_ERR),
                                                         request_timerfd_cb,
                                                         st,
                                                         NULL);

      guint64 now = monotonic_ns();

      if (now != 0)
        arm_timerfd_abs_ns(st->request_timer_fd, now + 1000000ull);
    } else {
      g_warning("timerfd_create failed: %s", g_strerror(errno));
    }
  }

  request_frame_tick(st);

  if (st->request_timer_fd < 0) {
    if (!st->request_timer_id)
      st->request_timer_id = g_timeout_add(REQUEST_INTERVAL_MS,
                                           request_frame_tick,
                                           st);
  }
}

static void
cb_vblank_rearm(StreamState *st)
{
  if (!st)
    return;

  if (st->backend != STREAM_BACKEND_NATIVE_BUFFER)
    stream_rearm_request_timer(st);
}

static void
cb_render_pending(StreamState *st)
{
  render_or_defer(st);
}

static gboolean
create_session(StreamState *st)
{
  GVariantBuilder b;

  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));

  GVariant *props = g_variant_ref_sink(g_variant_builder_end(&b));

  g_autoptr(GVariant) ret = call_sync(st->bus,
                                      IFACE_SC,
                                      IFACE_PATH,
                                      IFACE_SC,
                                      "CreateSession",
                                      g_variant_new("(@a{sv})", props));
  g_variant_unref(props);

  if (!ret)
    return FALSE;

  const char *session_path_tmp = NULL;

  g_variant_get(ret, "(&o)", &session_path_tmp);
  st->session_path = g_strdup(session_path_tmp);

  g_print("[DBus] CreateSession -> %s\n", st->session_path);

  return TRUE;
}

static gboolean
create_stream(StreamState *st)
{
  GVariantBuilder b;

  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));

  if (st->backend_override != STREAM_BACKEND_UNKNOWN)
    g_variant_builder_add(&b,
                          "{sv}",
                          "backend",
                          g_variant_new_string(backend_name(st->backend_override)));

  GVariant *props = g_variant_ref_sink(g_variant_builder_end(&b));

  g_autoptr(GVariant) ret = call_sync(st->bus,
                                      IFACE_SC,
                                      st->session_path,
                                      IFACE_SESSION,
                                      "CreateStream",
                                      g_variant_new("(@a{sv})", props));
  g_variant_unref(props);

  if (!ret)
    return FALSE;

  const char *stream_path_tmp = NULL;

  g_variant_get(ret, "(&o)", &stream_path_tmp);
  st->stream_path = g_strdup(stream_path_tmp);

  g_print("[DBus] CreateStream -> %s\n", st->stream_path);

  return TRUE;
}

static gboolean
start_session(StreamState *st)
{
  g_autoptr(GVariant) ret = call_sync(st->bus,
                                      IFACE_SC,
                                      st->session_path,
                                      IFACE_SESSION,
                                      "Start",
                                      NULL);
  if (!ret)
    return FALSE;

  g_print("[DBus] Start ok (session=%s)\n", st->session_path);

  return TRUE;
}

static void
detect_backend(StreamState *st)
{
  g_autoptr(GVariant) info = NULL;

  if (get_info_dbus(st->bus, st->stream_path, st, &info))
    backend_from_info(info, st);
  else
    st->backend = STREAM_BACKEND_MEMFD;

  if (st->backend_override != STREAM_BACKEND_UNKNOWN &&
      st->backend != st->backend_override)
    g_warning("[backend] requested %s but Mutter created %s",
              backend_name(st->backend_override),
              backend_name(st->backend));
}

static gboolean
setup_backend(StreamState *st)
{
  if (st->backend == STREAM_BACKEND_NATIVE_BUFFER)
    return setup_native_buffer_backend(st);

  st->backend = STREAM_BACKEND_MEMFD;

  if (!setup_memfd_backend(st))
    return FALSE;

  if (st->memfd.hdr) {
    st->info_width = st->memfd.hdr->width;
    st->info_height = st->memfd.hdr->height;
  }

  return TRUE;
}

static void
setup_stream_signals(StreamState *st)
{
  st->signal_sub_id = g_dbus_connection_signal_subscribe(st->bus,
                                                         IFACE_SC,
                                                         IFACE_STREAM,
                                                         "FrameReady",
                                                         st->stream_path,
                                                         NULL,
                                                         G_DBUS_SIGNAL_FLAGS_NONE,
                                                         on_frame_ready,
                                                         st,
                                                         NULL);

  st->signal_sub_damage_id = g_dbus_connection_signal_subscribe(st->bus,
                                                                IFACE_SC,
                                                                IFACE_STREAM,
                                                                "FrameReadyWithDamage",
                                                                st->stream_path,
                                                                NULL,
                                                                G_DBUS_SIGNAL_FLAGS_NONE,
                                                                on_frame_ready_with_damage,
                                                                st,
                                                                NULL);

  g_print("[DBus] subscribed FrameReady (id=%u) and FrameReadyWithDamage (id=%u)\n",
          st->signal_sub_id,
          st->signal_sub_damage_id);
}

static void
setup_stream_pacing(StreamState *st)
{
  st->need_render_after_flip = FALSE;

  st->vblank_valid = FALSE;
  st->vblank_last_ns = 0;

  if (st->vblank_lead_ns == 0)
    st->vblank_lead_ns = 2000000;

  pending_damage_set_full(st);

  st->render_pending_cb = cb_render_pending;
  st->vblank_cb = cb_vblank_rearm;
}

static void
connect_and_prepare_stream(StreamState *st)
{
  if (!st || !st->bus)
    return;

  stream_cleanup(st);

  if (!create_session(st))
    return;

  if (!create_stream(st))
    return;

  if (!start_session(st))
    return;

  detect_backend(st);

  if (!setup_backend(st)) {
    stream_cleanup(st);
    return;
  }

  ensure_drm_ready(st);

  if (st->backend == STREAM_BACKEND_NATIVE_BUFFER) {
    if (!drm_native_buffer_import_all(st)) {
      stream_cleanup(st);
      return;
    }
  }

  setup_stream_pacing(st);
  setup_stream_signals(st);

  render_or_defer(st);
  start_streaming(st);
}

static gboolean
delayed_start_cb(gpointer data)
{
  StreamState *st = data;

  st->delayed_start_id = 0;

  if (!st->service_present)
    return G_SOURCE_REMOVE;

  connect_and_prepare_stream(st);
  return G_SOURCE_REMOVE;
}

static void
schedule_delayed_start(StreamState *st)
{
  if (st->delayed_start_id) {
    g_source_remove(st->delayed_start_id);
    st->delayed_start_id = 0;
  }

  st->delayed_start_id = g_timeout_add(5000, delayed_start_cb, st);
}

static void
on_name_appeared(GDBusConnection *connection,
                 const gchar     *name,
                 const gchar     *name_owner,
                 gpointer         user_data)
{
  (void)connection;

  StreamState *st = user_data;

  g_print("[client] service appeared: %s owner=%s\n",
          name,
          name_owner ? name_owner : "(null)");

  st->service_present = TRUE;
  schedule_delayed_start(st);
}

static void
on_name_vanished(GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  (void)connection;

  StreamState *st = user_data;

  g_print("[client] service vanished: %s\n", name);

  st->service_present = FALSE;
  stream_cleanup(st);
}

void
setup_bus_and_watch(StreamState *st)
{
  g_autoptr(GError) error = NULL;

  st->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!st->bus) {
    g_warning("g_bus_get_sync failed: %s",
              error ? error->message : "unknown error");
    return;
  }

  g_print("[DBus] connected to session bus\n");

  st->name_watch_id = g_bus_watch_name_on_connection(st->bus,
                                                     IFACE_SC,
                                                     G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                     on_name_appeared,
                                                     on_name_vanished,
                                                     st,
                                                     NULL);

  g_print("[DBus] watching name %s (watch_id=%u)\n", IFACE_SC, st->name_watch_id);
}

void
stream_cleanup(StreamState *st)
{
  if (!st)
    return;

  st->streaming = FALSE;
  st->request_in_flight = FALSE;
  st->request_again = FALSE;

  if (st->request_timer_id) {
    g_source_remove(st->request_timer_id);
    st->request_timer_id = 0;
  }

  if (st->request_timer_source_id) {
    g_source_remove(st->request_timer_source_id);
    st->request_timer_source_id = 0;
  }

  if (st->request_timer_fd >= 0) {
    close(st->request_timer_fd);
    st->request_timer_fd = -1;
  }

  if (st->signal_sub_id && st->bus) {
    g_print("[DBus] unsub FrameReady (id=%u)\n", st->signal_sub_id);
    g_dbus_connection_signal_unsubscribe(st->bus, st->signal_sub_id);
    st->signal_sub_id = 0;
  }

  if (st->signal_sub_damage_id && st->bus) {
    g_print("[DBus] unsub FrameReadyWithDamage (id=%u)\n", st->signal_sub_damage_id);
    g_dbus_connection_signal_unsubscribe(st->bus, st->signal_sub_damage_id);
    st->signal_sub_damage_id = 0;
  }

  if (st->pending_damage) {
    g_array_free(st->pending_damage, TRUE);
    st->pending_damage = NULL;
  }

  if (st->memfd.map_base && st->memfd.map_base != MAP_FAILED) {
    munmap(st->memfd.map_base, st->memfd.map_len);
    st->memfd.map_base = NULL;
    st->memfd.map_len = 0;
  }

  if (st->memfd.fd >= 0) {
    close(st->memfd.fd);
    st->memfd.fd = -1;
  }

  st->memfd.hdr = NULL;

  drm_native_buffer_cleanup(st);

  if (st->native.info) {
    g_variant_unref(st->native.info);
    st->native.info = NULL;
  }

  if (st->native.fds) {
    for (int i = 0; i < st->native.n_fds; i++) {
      if (st->native.fds[i] >= 0)
        close(st->native.fds[i]);
    }

    g_free(st->native.fds);
    st->native.fds = NULL;
    st->native.n_fds = 0;
  }

  st->backend = STREAM_BACKEND_UNKNOWN;
  st->info_width = 0;
  st->info_height = 0;
  st->info_fps = 0.0;

  st->last_seen_seq = 0;
  st->last_presented_seq = 0;
  st->inflight_flip_seq = 0;
  st->pending_seq = 0;
  st->pending_slot = 0;

  st->need_render_after_flip = FALSE;
  st->force_full_damage = TRUE;

  st->vblank_valid = FALSE;
  st->vblank_last_ns = 0;

  st->render_pending_cb = NULL;
  st->vblank_cb = NULL;

  if (st->memfd.cpu_buf) {
    g_free(st->memfd.cpu_buf);
    st->memfd.cpu_buf = NULL;
    st->memfd.cpu_buf_len = 0;
  }

  g_clear_pointer(&st->session_path, g_free);
  g_clear_pointer(&st->stream_path, g_free);

  drm_cleanup(&st->sink);
}

void
cleanup_all(StreamState *st)
{
  if (!st)
    return;

  stream_cleanup(st);

  if (st->name_watch_id) {
    g_print("[DBus] unwatch name (watch_id=%u)\n", st->name_watch_id);
    g_bus_unwatch_name(st->name_watch_id);
    st->name_watch_id = 0;
  }

  if (st->delayed_start_id) {
    g_source_remove(st->delayed_start_id);
    st->delayed_start_id = 0;
  }

  g_clear_object(&st->bus);
}
