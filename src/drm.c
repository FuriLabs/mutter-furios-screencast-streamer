/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <gio/gio.h>
#include <glib-unix.h>

#include <sys/mman.h>

#include "stream.h"
#include "memfd.h"
#include "drm.h"
#include "dbus.h"

static guint64
monotonic_ns(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;

  return (guint64)ts.tv_sec * 1000000000ull + (guint64)ts.tv_nsec;
}

static guint64
compute_vblank_period_ns(const drmModeModeInfo *m)
{
  if (!m)
    return 16666666ull;

  if (m->htotal == 0 || m->vtotal == 0 || m->clock == 0)
    return 16666666ull;

  double hz = ((double)m->clock * 1000.0) / ((double)m->htotal * (double)m->vtotal);
  if (hz < 1.0)
    return 16666666ull;

  guint64 period = (guint64)(1000000000.0 / hz + 0.5);
  if (period == 0)
    period = 16666666ull;

  return period;
}

static const char *
conn_type_str(uint32_t t)
{
  switch (t) {
  case DRM_MODE_CONNECTOR_DVID:
    return "DVI-D";
  case DRM_MODE_CONNECTOR_DVII:
    return "DVI-I";
  case DRM_MODE_CONNECTOR_HDMIA:
    return "HDMI-A";
  case DRM_MODE_CONNECTOR_DisplayPort:
    return "DP";
  case DRM_MODE_CONNECTOR_eDP:
    return "eDP";
  default:
    return "CONN";
  }
}

static void
make_conn_name(char *out,
               size_t out_sz,
               drmModeConnector *conn)
{
  snprintf(out, out_sz, "%s-%u",
           conn_type_str(conn->connector_type),
           conn->connector_type_id);
}

static void
drm_buffer_destroy(int drm_fd,
                   DrmBuffer *b)
{
  if (!b)
    return;

  if (b->map && b->map != MAP_FAILED) {
    munmap(b->map, (size_t)b->size);
    b->map = NULL;
  }

  if (b->fb_id) {
    drmModeRmFB(drm_fd, b->fb_id);
    b->fb_id = 0;
  }

  if (b->handle) {
    struct drm_mode_destroy_dumb d;
    memset(&d, 0, sizeof(d));
    d.handle = b->handle;
    drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    b->handle = 0;
  }

  b->pitch = 0;
  b->size = 0;
  b->fb_w = 0;
  b->fb_h = 0;
}

void
drm_cleanup(DrmSink *s)
{
  if (!s)
    return;

  if (s->drm_source_id) {
    g_source_remove(s->drm_source_id);
    s->drm_source_id = 0;
  }

  if (s->drm_fd >= 0) {
    drm_buffer_destroy(s->drm_fd, &s->bufs[0]);
    drm_buffer_destroy(s->drm_fd, &s->bufs[1]);

    close(s->drm_fd);
    s->drm_fd = -1;
  }

  s->conn_id = 0;
  s->crtc_id = 0;
  s->have_mode = 0;

  s->front_idx = 0;
  s->pending_flip = 0;
  s->pending_flip_next_front = 0;

  s->fb_w = 0;
  s->fb_h = 0;
}

static int
pick_connector_mode_and_crtc(DrmSink *s,
                             uint32_t want_w,
                             uint32_t want_h)
{
  drmModeRes *res = drmModeGetResources(s->drm_fd);
  if (!res)
    return -1;

  int found = 0;

  for (int i = 0; i < res->count_connectors && !found; i++) {
    drmModeConnector *conn = drmModeGetConnector(s->drm_fd, res->connectors[i]);
    if (!conn)
      continue;

    char name[32];
    make_conn_name(name, sizeof(name), conn);

    int name_ok = 1;
    if (s->connector_want && *s->connector_want)
      name_ok = strcmp(name, s->connector_want) == 0;

    if (conn->connection == DRM_MODE_CONNECTED &&
        conn->count_modes > 0 &&
        name_ok) {

      s->conn_id = conn->connector_id;

      int best = 0;
      for (int m = 0; m < conn->count_modes; m++) {
        if ((uint32_t)conn->modes[m].hdisplay == want_w &&
            (uint32_t)conn->modes[m].vdisplay == want_h) {
          best = m;
          break;
        }
      }

      s->mode = conn->modes[best];
      s->have_mode = 1;

      uint32_t enc_id = conn->encoder_id;
      if (!enc_id && conn->count_encoders > 0)
        enc_id = conn->encoders[0];

      uint32_t crtc_id = 0;
      if (enc_id) {
        drmModeEncoder *enc = drmModeGetEncoder(s->drm_fd, enc_id);
        if (enc) {
          crtc_id = enc->crtc_id;
          drmModeFreeEncoder(enc);
        }
      }

      if (!crtc_id && res->count_crtcs > 0)
        crtc_id = res->crtcs[0];

      s->crtc_id = crtc_id;

      g_print("[drm] using %s %ux%u crtc=%u\n",
              name,
              s->mode.hdisplay,
              s->mode.vdisplay,
              s->crtc_id);

      found = 1;
    }

    drmModeFreeConnector(conn);
  }

  drmModeFreeResources(res);
  return found ? 0 : -1;
}

static int
create_dumb_xrgb8888_one(DrmSink *s,
                         DrmBuffer *b,
                         uint32_t w,
                         uint32_t h)
{
  struct drm_mode_create_dumb creq;
  memset(&creq, 0, sizeof(creq));
  creq.width = w;
  creq.height = h;
  creq.bpp = 32;

  if (drmIoctl(s->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
    return -1;

  b->handle = creq.handle;
  b->pitch = creq.pitch;
  b->size = creq.size;

  b->fb_w = w;
  b->fb_h = h;

  uint32_t handles[4] = { b->handle, 0, 0, 0 };
  uint32_t pitches[4] = { b->pitch, 0, 0, 0 };
  uint32_t offsets[4] = { 0, 0, 0, 0 };

  if (drmModeAddFB2(s->drm_fd,
                    w,
                    h,
                    DRM_FORMAT_XRGB8888,
                    handles,
                    pitches,
                    offsets,
                    &b->fb_id,
                    0) != 0) {
    if (drmModeAddFB(s->drm_fd, w, h, 24, 32, b->pitch, b->handle, &b->fb_id) != 0)
      return -1;
  }

  struct drm_mode_map_dumb mreq;
  memset(&mreq, 0, sizeof(mreq));
  mreq.handle = b->handle;

  if (drmIoctl(s->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
    return -1;

  b->map = mmap(NULL,
                (size_t)b->size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                s->drm_fd,
                mreq.offset);
  if (b->map == MAP_FAILED)
    return -1;

  memset(b->map, 0, (size_t)b->size);
  return 0;
}

static int
drm_set_mode(DrmSink *s,
             uint32_t fb_id)
{
  if (!s->have_mode)
    return -1;

  return drmModeSetCrtc(s->drm_fd,
                        s->crtc_id,
                        fb_id,
                        0,
                        0,
                        &s->conn_id,
                        1,
                        &s->mode);
}

static int32_t
clamp_i32(int32_t v,
          int32_t lo,
          int32_t hi)
{
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static void
sanitize_damage_rects(GArray *rects,
                      uint32_t w,
                      uint32_t h)
{
  if (!rects)
    return;

  if (rects->len == 0) {
    DamageRect r;
    r.x = 0;
    r.y = 0;
    r.w = (int32_t)w;
    r.h = (int32_t)h;
    g_array_append_val(rects, r);
    return;
  }

  for (guint i = 0; i < rects->len; i++) {
    DamageRect *r = &g_array_index(rects, DamageRect, i);

    int32_t x1 = r->x;
    int32_t y1 = r->y;
    int32_t x2 = r->x + r->w;
    int32_t y2 = r->y + r->h;

    x1 = clamp_i32(x1, 0, (int32_t)w);
    y1 = clamp_i32(y1, 0, (int32_t)h);
    x2 = clamp_i32(x2, 0, (int32_t)w);
    y2 = clamp_i32(y2, 0, (int32_t)h);

    r->x = x1;
    r->y = y1;
    r->w = x2 - x1;
    r->h = y2 - y1;

    if (r->w < 0)
      r->w = 0;
    if (r->h < 0)
      r->h = 0;
  }
}

static gboolean
is_full_damage(GArray *rects,
               uint32_t w,
               uint32_t h)
{
  if (!rects)
    return FALSE;

  if (rects->len != 1)
    return FALSE;

  DamageRect r = g_array_index(rects, DamageRect, 0);
  if (r.x != 0)
    return FALSE;
  if (r.y != 0)
    return FALSE;
  if ((uint32_t)r.w != w)
    return FALSE;
  if ((uint32_t)r.h != h)
    return FALSE;

  return TRUE;
}

static gboolean
get_slot_base(StreamState *st,
              const MetaFuriosMemfdHeader *h,
              const uint8_t **out_slot_base)
{
  if (!st || !h || !out_slot_base)
    return FALSE;

  const uint32_t n_slots = h->n_slots ? h->n_slots : 1;
  const uint32_t slot_bytes = h->slot_bytes;
  const uint32_t header_bytes = h->header_bytes;

  if (slot_bytes == 0)
    return FALSE;
  if (header_bytes < sizeof(MetaFuriosMemfdHeader))
    return FALSE;

  uint32_t slot = 0;
  if (n_slots)
    slot = st->pending_slot % n_slots;

  size_t off = (size_t)header_bytes + (size_t)slot * (size_t)slot_bytes;
  if (off + (size_t)slot_bytes > st->map_len)
    return FALSE;

  *out_slot_base = (const uint8_t *)st->map_base + off;
  return TRUE;
}

static gboolean
copy_damage_direct_bxgx_to_back(StreamState *st,
                                DrmBuffer *front,
                                DrmBuffer *back,
                                const MetaFuriosMemfdHeader *h,
                                GArray *damage_rects)
{
  if (!st || !front || !back || !h || !damage_rects)
    return FALSE;

  uint32_t w = h->width;
  uint32_t hh = h->height;

  if (w > back->fb_w)
    w = back->fb_w;
  if (hh > back->fb_h)
    hh = back->fb_h;

  sanitize_damage_rects(damage_rects, w, hh);

  gboolean full = is_full_damage(damage_rects, w, hh);

  const uint32_t src_stride = h->stride;
  const uint32_t dst_stride = back->pitch;

  for (int tries = 0; tries < 6; tries++) {
    uint32_t seq_a = __atomic_load_n(&st->hdr->seq, __ATOMIC_ACQUIRE);

    const uint8_t *slot_base = NULL;
    if (!get_slot_base(st, h, &slot_base))
      return FALSE;

    if (!full) {
      size_t n = (size_t)front->size;
      if ((size_t)back->size < n)
        n = (size_t)back->size;
      memcpy(back->map, front->map, n);
    }

    if (full &&
        src_stride == dst_stride &&
        h->width == w &&
        h->height == hh) {

      size_t bytes = (size_t)hh * (size_t)src_stride;
      if (bytes > (size_t)back->size)
        bytes = (size_t)back->size;

      memcpy(back->map, slot_base, bytes);

      if ((size_t)back->size > bytes)
        memset((uint8_t *)back->map + bytes, 0, (size_t)back->size - bytes);
    } else {
      for (guint i = 0; i < damage_rects->len; i++) {
        DamageRect r = g_array_index(damage_rects, DamageRect, i);

        if (r.w <= 0 || r.h <= 0)
          continue;

        uint32_t rw = (uint32_t)r.w;
        uint32_t rh = (uint32_t)r.h;

        for (uint32_t yy = 0; yy < rh; yy++) {
          uint32_t y = (uint32_t)r.y + yy;
          const uint8_t *sp = slot_base + (size_t)y * (size_t)src_stride + (size_t)r.x * 4;
          uint8_t *dp = (uint8_t *)back->map + (size_t)y * (size_t)dst_stride + (size_t)r.x * 4;

          memcpy(dp, sp, (size_t)rw * 4);
        }
      }
    }

    uint32_t seq_b = __atomic_load_n(&st->hdr->seq, __ATOMIC_ACQUIRE);
    if (seq_a == seq_b)
      return TRUE;
  }

  return FALSE;
}

static void
ensure_cpu_buf(StreamState *st,
               size_t need)
{
  if (!st)
    return;

  if (need == 0)
    return;

  if (st->cpu_buf && st->cpu_buf_len == need)
    return;

  g_free(st->cpu_buf);
  st->cpu_buf = g_malloc0(need);
  st->cpu_buf_len = need;
}

static gboolean
snapshot_slot_to_cpu(StreamState *st)
{
  if (!st || !st->hdr || !st->map_base)
    return FALSE;

  if (!memfd_header_sane(st->hdr))
    return FALSE;

  const uint32_t n_slots = st->hdr->n_slots ? st->hdr->n_slots : 1;
  const uint32_t slot_bytes = st->hdr->slot_bytes;
  const uint32_t header_bytes = st->hdr->header_bytes;

  if (slot_bytes == 0 || header_bytes < sizeof(MetaFuriosMemfdHeader))
    return FALSE;

  ensure_cpu_buf(st, (size_t)slot_bytes);
  if (!st->cpu_buf)
    return FALSE;

  uint32_t seq_a = 0;
  uint32_t seq_b = 0;
  uint32_t slot = st->pending_slot % n_slots;

  for (int tries = 0; tries < 6; tries++) {
    seq_a = __atomic_load_n(&st->hdr->seq, __ATOMIC_ACQUIRE);
    slot = st->pending_slot % n_slots;

    size_t off = (size_t)header_bytes + (size_t)slot * (size_t)slot_bytes;
    if (off + (size_t)slot_bytes > st->map_len)
      return FALSE;

    const uint8_t *slot_base = (const uint8_t *)st->map_base + off;

    memcpy(st->cpu_buf, slot_base, (size_t)slot_bytes);

    seq_b = __atomic_load_n(&st->hdr->seq, __ATOMIC_ACQUIRE);
    if (seq_a == seq_b)
      return TRUE;
  }

  return FALSE;
}

static void
blit_rect_rgba_to_xrgb8888(DrmBuffer *dst,
                           const MetaFuriosMemfdHeader *h,
                           const uint8_t *src_full,
                           DamageRect r)
{
  uint8_t *dbase = (uint8_t *)dst->map;

  const uint32_t dst_stride = dst->pitch;
  const uint32_t src_stride = h->stride;

  const uint32_t rw = (uint32_t)r.w;
  const uint32_t rh = (uint32_t)r.h;

  for (uint32_t yy = 0; yy < rh; yy++) {
    uint32_t y = (uint32_t)r.y + yy;

    const uint32_t *sp32 = (const uint32_t *)(src_full + (size_t)y * (size_t)src_stride + (size_t)r.x * 4);
    uint32_t *dp32 = (uint32_t *)(dbase + (size_t)y * (size_t)dst_stride + (size_t)r.x * 4);

    for (uint32_t x = 0; x < rw; x++) {
      uint32_t v = sp32[x];
      uint32_t out = 0xFF000000u |
                     ((v & 0x000000FFu) << 16) |
                     (v & 0x0000FF00u) |
                     ((v & 0x00FF0000u) >> 16);
      dp32[x] = out;
    }
  }
}

static void
blit_damage_rgba_to_xrgb8888(DrmBuffer *dst,
                             const MetaFuriosMemfdHeader *h,
                             const uint8_t *src_full,
                             GArray *damage_rects)
{
  if (!dst || !dst->map || !h || !src_full || !damage_rects)
    return;

  uint32_t w = h->width;
  uint32_t hh = h->height;

  if (w > dst->fb_w)
    w = dst->fb_w;
  if (hh > dst->fb_h)
    hh = dst->fb_h;

  sanitize_damage_rects(damage_rects, w, hh);

  for (guint i = 0; i < damage_rects->len; i++) {
    DamageRect r = g_array_index(damage_rects, DamageRect, i);

    if (r.w <= 0 || r.h <= 0)
      continue;

    blit_rect_rgba_to_xrgb8888(dst, h, src_full, r);
  }
}

static void
drm_page_flip_handler(int fd,
                      unsigned int frame,
                      unsigned int sec,
                      unsigned int usec,
                      void *data)
{
  (void)fd;
  (void)frame;
  (void)sec;
  (void)usec;

  StreamState *st = data;
  if (!st)
    return;

  DrmSink *s = &st->sink;
  s->pending_flip = 0;
  s->front_idx = s->pending_flip_next_front;

  if (st->inflight_flip_seq != 0)
    st->last_presented_seq = st->inflight_flip_seq;

  st->inflight_flip_seq = 0;

  guint64 now = monotonic_ns();
  if (now != 0) {
    st->vblank_last_ns = now;
    st->vblank_valid = TRUE;
    stream_rearm_request_timer(st);
  }

  if (st->need_render_after_flip) {
    st->need_render_after_flip = FALSE;
    render_frame_drm(st);
  }
}

static gboolean
drm_fd_ready_cb(gint fd,
                GIOCondition cond,
                gpointer user_data)
{
  (void)fd;

  StreamState *st = user_data;
  if (!st)
    return G_SOURCE_CONTINUE;

  if (!(cond & G_IO_IN))
    return G_SOURCE_CONTINUE;

  drmEventContext ev;
  memset(&ev, 0, sizeof(ev));
  ev.version = DRM_EVENT_CONTEXT_VERSION;
  ev.page_flip_handler = drm_page_flip_handler;

  drmHandleEvent(st->sink.drm_fd, &ev);
  return G_SOURCE_CONTINUE;
}

void
ensure_drm_ready(StreamState *st)
{
  DrmSink *s = &st->sink;

  if (s->drm_fd >= 0 &&
      s->bufs[0].fb_id &&
      s->bufs[1].fb_id &&
      s->bufs[0].map &&
      s->bufs[1].map)
    return;

  char path[64];
  snprintf(path, sizeof(path), "/dev/dri/card%d", s->card_index);

  s->drm_fd = open(path, O_RDWR | O_CLOEXEC);
  if (s->drm_fd < 0) {
    g_warning("open %s failed: %s", path, g_strerror(errno));
    return;
  }

  for (int tries = 0; tries < 50; tries++) {
    uint32_t want_w = st->hdr ? st->hdr->width : 1920;
    uint32_t want_h = st->hdr ? st->hdr->height : 1080;

    if (pick_connector_mode_and_crtc(s, want_w, want_h) == 0)
      break;

    usleep(100 * 1000);
  }

  if (!s->have_mode) {
    if (s->connector_want && *s->connector_want)
      g_warning("[drm] no CONNECTED connector matching '%s'", s->connector_want);
    else
      g_warning("[drm] no CONNECTED connector found");

    drm_cleanup(s);
    return;
  }

  if (create_dumb_xrgb8888_one(s, &s->bufs[0], s->mode.hdisplay, s->mode.vdisplay) != 0) {
    g_warning("[drm] create dumb buffer 0 failed: %s", g_strerror(errno));
    drm_cleanup(s);
    return;
  }

  if (create_dumb_xrgb8888_one(s, &s->bufs[1], s->mode.hdisplay, s->mode.vdisplay) != 0) {
    g_warning("[drm] create dumb buffer 1 failed: %s", g_strerror(errno));
    drm_cleanup(s);
    return;
  }

  s->fb_w = s->mode.hdisplay;
  s->fb_h = s->mode.vdisplay;

  s->front_idx = 0;
  s->pending_flip = 0;
  s->pending_flip_next_front = 0;

  if (drm_set_mode(s, s->bufs[s->front_idx].fb_id) != 0) {
    g_warning("[drm] drmModeSetCrtc failed: %s", g_strerror(errno));
    drm_cleanup(s);
    return;
  }

  if (st) {
    st->vblank_period_ns = compute_vblank_period_ns(&s->mode);
    if (st->vblank_lead_ns == 0)
      st->vblank_lead_ns = 2000000;
    if (st->vblank_lead_ns > st->vblank_period_ns / 2)
      st->vblank_lead_ns = st->vblank_period_ns / 2;
  }

  if (!s->drm_source_id)
    s->drm_source_id = g_unix_fd_add_full(G_PRIORITY_HIGH,
                                          s->drm_fd,
                                          (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                          drm_fd_ready_cb,
                                          st,
                                          NULL);

  g_print("[drm] ready: %ux%u front_fb=%u pitch=%u\n",
          s->mode.hdisplay,
          s->mode.vdisplay,
          s->bufs[s->front_idx].fb_id,
          s->bufs[s->front_idx].pitch);
}

void
render_frame_drm(StreamState *st)
{
  if (!st || !st->hdr)
    return;

  ensure_drm_ready(st);
  DrmSink *s = &st->sink;

  if (s->drm_fd < 0)
    return;
  if (!s->bufs[0].fb_id || !s->bufs[1].fb_id)
    return;
  if (!s->bufs[0].map || !s->bufs[1].map)
    return;

  if (s->pending_flip) {
    st->need_render_after_flip = TRUE;
    return;
  }

  if (!st->pending_damage) {
    st->pending_damage = g_array_new(FALSE, FALSE, sizeof(DamageRect));
    g_array_set_size(st->pending_damage, 0);

    DamageRect r;
    r.x = 0;
    r.y = 0;
    r.w = (int32_t)st->hdr->width;
    r.h = (int32_t)st->hdr->height;
    g_array_append_val(st->pending_damage, r);
  }

  MetaFuriosMemfdHeader tmp = *st->hdr;

  if (tmp.width > s->fb_w)
    tmp.width = s->fb_w;
  if (tmp.height > s->fb_h)
    tmp.height = s->fb_h;

  int back_idx = 1 - s->front_idx;

  DrmBuffer *front = &s->bufs[s->front_idx];
  DrmBuffer *back = &s->bufs[back_idx];

  sanitize_damage_rects(st->pending_damage, tmp.width, tmp.height);

  gboolean ok = FALSE;

  if (tmp.format == META_FURIOS_MEMFD_FORMAT_BGRX8888 ||
      tmp.format == META_FURIOS_MEMFD_FORMAT_BGRA8888) {
    ok = copy_damage_direct_bxgx_to_back(st, front, back, &tmp, st->pending_damage);
    if (!ok)
      return;
  } else {
    if (!snapshot_slot_to_cpu(st))
      return;

    size_t n = (size_t)front->size;
    if ((size_t)back->size < n)
      n = (size_t)back->size;

    gboolean full = is_full_damage(st->pending_damage, tmp.width, tmp.height);
    if (!full)
      memcpy(back->map, front->map, n);

    blit_damage_rgba_to_xrgb8888(back, &tmp, st->cpu_buf, st->pending_damage);
  }

  int ret = drmModePageFlip(s->drm_fd,
                            s->crtc_id,
                            back->fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT,
                            st);
  if (ret != 0) {
    g_warning("[drm] drmModePageFlip failed: %s", g_strerror(errno));
    return;
  }

  s->pending_flip = 1;
  s->pending_flip_next_front = back_idx;

  st->inflight_flip_seq = st->pending_seq;
  st->force_full_damage = FALSE;
}
