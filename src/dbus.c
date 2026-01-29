/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <glib-unix.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>

#include "stream.h"
#include "dbus.h"
#include "memfd.h"
#include "drm.h"

static guint64
monotonic_ns(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;

  return (guint64)ts.tv_sec * 1000000000ull + (guint64)ts.tv_nsec;
}

static guint64
clamp_u64(guint64 v,
          guint64 lo,
          guint64 hi)
{
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static void
arm_timerfd_abs_ns(int fd,
                   guint64 when_ns)
{
  struct itimerspec its;
  memset(&its, 0, sizeof(its));

  its.it_value.tv_sec = (time_t)(when_ns / 1000000000ull);
  its.it_value.tv_nsec = (long)(when_ns % 1000000000ull);

  if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL) != 0)
    g_warning("timerfd_settime failed: %s", g_strerror(errno));
}

void
stream_rearm_request_timer(StreamState *st)
{
  if (!st)
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

static int
get_memfd_dbus(GDBusConnection *bus,
                   const char *stream_path)
{
  g_autoptr(GError) err = NULL;
  g_autoptr(GUnixFDList) out_fds = NULL;

  g_debug("[GetMemfd] calling GetMemfd on %s", stream_path);

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

  g_debug("[GetMemfd] got fd=%d size=%zu", fd, (size_t)stbuf.st_size);
  return fd;
}

static void
on_request_frame_done(GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
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
    return;
  }

  g_variant_unref(ret);

  if (st->request_timer_fd >= 0)
    stream_rearm_request_timer(st);
}

static void
request_next_frame(StreamState *st)
{
  if (!st || !st->bus || !st->stream_path)
    return;
  if (!st->streaming)
    return;
  if (st->request_in_flight)
    return;

  st->request_in_flight = TRUE;

  g_dbus_connection_call(st->bus,
                         IFACE_SC,
                         st->stream_path,
                         IFACE_STREAM,
                         "RequestFrame",
                         NULL,
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         2000,
                         NULL,
                         on_request_frame_done,
                         st);
}

static gboolean
request_timerfd_cb(gint fd,
                   GIOCondition cond,
                   gpointer user_data)
{
  (void)cond;

  StreamState *st = user_data;
  if (!st)
    return G_SOURCE_CONTINUE;

  uint64_t expirations = 0;
  ssize_t n = read(fd, &expirations, sizeof(expirations));
  if (n < 0) {
    if (errno != EAGAIN)
      g_warning("timerfd read failed: %s", g_strerror(errno));
  }

  request_next_frame(st);

  if (st->request_in_flight) {
    guint64 now = monotonic_ns();
    if (now != 0)
      arm_timerfd_abs_ns(st->request_timer_fd, now + 2000000ull);
  } else {
    stream_rearm_request_timer(st);
  }

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

  request_next_frame(st);
  return G_SOURCE_CONTINUE;
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

  if (!st->hdr)
    return;

  DamageRect r;
  r.x = 0;
  r.y = 0;
  r.w = (int32_t)st->hdr->width;
  r.h = (int32_t)st->hdr->height;

  g_array_append_val(st->pending_damage, r);
}

static void
pending_damage_replace(StreamState *st,
                        GVariant *damage)
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

  render_frame_drm(st);
}

static void
on_frame_ready_common(StreamState *st,
                      guint32 seq,
                      guint32 slot,
                      GVariant *damage_maybe)
{
  if (!st || !st->hdr)
    return;

  if (seq == st->last_seen_seq)
    return;

  if (st->sink.pending_flip)
    st->force_full_damage = TRUE;

  if (st->last_presented_seq != 0) {
    if (seq > st->last_presented_seq + 1)
      st->force_full_damage = TRUE;
  }

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
               const gchar *sender_name,
               const gchar *object_path,
               const gchar *interface_name,
               const gchar *signal_name,
               GVariant *parameters,
               gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;

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
                           const gchar *sender_name,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *signal_name,
                           GVariant *parameters,
                           gpointer user_data)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;

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

static void
start_streaming(StreamState *st)
{
  if (!st || st->streaming)
    return;

  st->streaming = TRUE;
  st->request_in_flight = FALSE;

  if (st->request_timer_fd < 0) {
    st->request_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (st->request_timer_fd >= 0) {
      if (!st->request_timer_source_id)
        st->request_timer_source_id = g_unix_fd_add_full(G_PRIORITY_HIGH,
                                                         st->request_timer_fd,
                                                         (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
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
      st->request_timer_id = g_timeout_add(REQUEST_INTERVAL_MS, request_frame_tick, st);
  }
}

static void
connect_and_prepare_stream(StreamState *st)
{
  if (!st || !st->bus)
    return;

  stream_cleanup(st);

  g_print("[setup] CreateSession()\n");

  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  GVariant *props = g_variant_builder_end(&b);

  g_autoptr(GVariant) ret_create_session = call_sync(st->bus,
                                                     IFACE_SC,
                                                     IFACE_PATH,
                                                     IFACE_SC,
                                                     "CreateSession",
                                                     g_variant_new("(@a{sv})", props));
  if (!ret_create_session)
    return;

  const char *session_path_tmp = NULL;
  g_variant_get(ret_create_session, "(&o)", &session_path_tmp);
  st->session_path = g_strdup(session_path_tmp);

  g_print("Session: %s\n", st->session_path);
  g_print("[setup] CreateStream()\n");

  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  GVariant *sprops = g_variant_builder_end(&b);

  g_autoptr(GVariant) ret_create_stream = call_sync(st->bus,
                                                    IFACE_SC,
                                                    st->session_path,
                                                    IFACE_SESSION,
                                                    "CreateStream",
                                                    g_variant_new("(@a{sv})", sprops));
  if (!ret_create_stream)
    return;

  const char *stream_path_tmp = NULL;
  g_variant_get(ret_create_stream, "(&o)", &stream_path_tmp);
  st->stream_path = g_strdup(stream_path_tmp);

  g_print("Stream: %s\n", st->stream_path);
  g_print("[setup] Start()\n");

  g_autoptr(GVariant) ret_start = call_sync(st->bus,
                                           IFACE_SC,
                                           st->session_path,
                                           IFACE_SESSION,
                                           "Start",
                                           NULL);
  if (!ret_start)
    return;

  st->memfd = get_memfd_dbus(st->bus, st->stream_path);
  if (st->memfd < 0)
    return;

  struct stat stbuf;
  if (fstat(st->memfd, &stbuf) != 0) {
    g_warning("fstat(memfd) failed: %s", g_strerror(errno));
    stream_cleanup(st);
    return;
  }

  st->map_len = (size_t)stbuf.st_size;
  st->map_base = mmap(NULL,
                      st->map_len,
                      PROT_READ,
                      MAP_SHARED,
                      st->memfd,
                      0);
  if (st->map_base == MAP_FAILED) {
    g_warning("mmap(memfd) failed: %s", g_strerror(errno));
    stream_cleanup(st);
    return;
  }

  st->hdr = (MetaFuriosMemfdHeader *)st->map_base;
  memfd_dump_header("[initial]", st->hdr);

  if (!memfd_header_sane(st->hdr)) {
    g_warning("invalid memfd header");
    stream_cleanup(st);
    return;
  }

  st->last_seen_seq = st->hdr->seq;
  st->last_presented_seq = st->hdr->seq;
  st->inflight_flip_seq = 0;
  st->force_full_damage = TRUE;

  st->pending_seq = st->last_seen_seq;
  st->pending_slot = st->hdr->last_slot;

  st->need_render_after_flip = FALSE;

  st->vblank_valid = FALSE;
  st->vblank_last_ns = 0;
  if (st->vblank_lead_ns == 0)
    st->vblank_lead_ns = 2000000;

  pending_damage_set_full(st);

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

  ensure_drm_ready(st);

  if (st->request_timer_fd >= 0)
    stream_rearm_request_timer(st);

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
                 const gchar *name,
                 const gchar *name_owner,
                 gpointer user_data)
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
                 const gchar *name,
                 gpointer user_data)
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

  g_print("[setup] connecting to session bus...\n");

  st->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!st->bus) {
    g_warning("g_bus_get_sync failed: %s",
              error ? error->message : "unknown error");
    return;
  }

  st->name_watch_id = g_bus_watch_name_on_connection(st->bus,
                                                     IFACE_SC,
                                                     G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                     on_name_appeared,
                                                     on_name_vanished,
                                                     st,
                                                     NULL);
}

void
stream_cleanup(StreamState *st)
{
  if (!st)
    return;

  st->streaming = FALSE;
  st->request_in_flight = FALSE;

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
    g_dbus_connection_signal_unsubscribe(st->bus, st->signal_sub_id);
    st->signal_sub_id = 0;
  }

  if (st->signal_sub_damage_id && st->bus) {
    g_dbus_connection_signal_unsubscribe(st->bus, st->signal_sub_damage_id);
    st->signal_sub_damage_id = 0;
  }

  if (st->pending_damage) {
    g_array_free(st->pending_damage, TRUE);
    st->pending_damage = NULL;
  }

  if (st->map_base && st->map_base != MAP_FAILED) {
    munmap(st->map_base, st->map_len);
    st->map_base = NULL;
    st->map_len = 0;
  }

  if (st->memfd >= 0) {
    close(st->memfd);
    st->memfd = -1;
  }

  st->hdr = NULL;
  st->last_seen_seq = 0;
  st->last_presented_seq = 0;
  st->inflight_flip_seq = 0;
  st->pending_seq = 0;
  st->pending_slot = 0;
  st->need_render_after_flip = FALSE;
  st->force_full_damage = TRUE;

  st->vblank_valid = FALSE;
  st->vblank_last_ns = 0;

  if (st->cpu_buf) {
    g_free(st->cpu_buf);
    st->cpu_buf = NULL;
    st->cpu_buf_len = 0;
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
    g_bus_unwatch_name(st->name_watch_id);
    st->name_watch_id = 0;
  }

  if (st->delayed_start_id) {
    g_source_remove(st->delayed_start_id);
    st->delayed_start_id = 0;
  }

  g_clear_object(&st->bus);
}
