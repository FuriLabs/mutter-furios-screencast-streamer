/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "drm_utils.h"
#include "stream.h"

#include <sys/mman.h>
#include <glib-unix.h>

static guint64
compute_vblank_period_ns_internal(const drmModeModeInfo *m)
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

guint64
drm_compute_vblank_period_ns(const drmModeModeInfo *m)
{
  return compute_vblank_period_ns_internal(m);
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
make_conn_name(char             *out,
               size_t            out_sz,
               drmModeConnector *conn)
{
  snprintf(out, out_sz, "%s-%u",
           conn_type_str(conn->connector_type),
           conn->connector_type_id);
}

static void
drm_buffer_destroy(int        drm_fd,
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
pick_connector_mode_and_crtc(DrmSink  *s,
                             uint32_t  want_w,
                             uint32_t  want_h)
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
create_dumb_xrgb8888_one(DrmSink   *s,
                         DrmBuffer *b,
                         uint32_t   w,
                         uint32_t   h)
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

static void
drm_page_flip_handler(int           fd,
                      unsigned int  frame,
                      unsigned int  sec,
                      unsigned int  usec,
                      void         *data)
{
  (void)fd;
  (void)frame;

  StreamState *st = data;
  if (!st)
    return;

  DrmSink *s = &st->sink;

  s->pending_flip = 0;
  s->front_idx = s->pending_flip_next_front;

  if (st->inflight_flip_seq != 0)
    st->last_presented_seq = st->inflight_flip_seq;

  st->inflight_flip_seq = 0;

  guint64 vblank_ns = 0;
  if (sec != 0 || usec != 0)
    vblank_ns = ((guint64)sec * 1000000000ull) + ((guint64)usec * 1000ull);

  if (vblank_ns != 0) {
    st->vblank_last_ns = vblank_ns;
    st->vblank_valid = TRUE;

    if (st->vblank_cb)
      st->vblank_cb(st);
  }

  if (st->need_render_after_flip) {
    st->need_render_after_flip = FALSE;

    if (st->render_pending_cb)
      st->render_pending_cb(st);
  }

  if (st->flip_complete_cb)
    st->flip_complete_cb(st);
}

static gboolean
drm_fd_ready_cb(gint         fd,
                GIOCondition cond,
                gpointer     user_data)
{
  StreamState *st = user_data;

  if (!st)
    return G_SOURCE_REMOVE;

  if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
    g_warning("[drm] fd=%d error condition: 0x%x",
              fd,
              (unsigned int)cond);

    st->sink.drm_source_id = 0;

    return G_SOURCE_REMOVE;
  }

  if (!(cond & G_IO_IN))
    return G_SOURCE_CONTINUE;

  drmEventContext ev;

  memset(&ev, 0, sizeof(ev));
  ev.version = DRM_EVENT_CONTEXT_VERSION;
  ev.page_flip_handler = drm_page_flip_handler;

  if (drmHandleEvent(st->sink.drm_fd, &ev) != 0)
    g_warning("[drm] drmHandleEvent failed: %s",
              g_strerror(errno));

  return G_SOURCE_CONTINUE;
}

static void
get_wanted_mode_size(StreamState *st,
                     uint32_t    *out_w,
                     uint32_t    *out_h)
{
  uint32_t w = 1920;
  uint32_t h = 1080;

  if (st) {
    if (st->backend == STREAM_BACKEND_NATIVE_BUFFER) {
      if (st->native_width)
        w = st->native_width;
      else if (st->info_width)
        w = st->info_width;

      if (st->native_height)
        h = st->native_height;
      else if (st->info_height)
        h = st->info_height;
    } else {
      if (st->hdr && st->hdr->width)
        w = st->hdr->width;
      else if (st->info_width)
        w = st->info_width;

      if (st->hdr && st->hdr->height)
        h = st->hdr->height;
      else if (st->info_height)
        h = st->info_height;
    }
  }

  *out_w = w;
  *out_h = h;
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
    uint32_t want_w = 1920;
    uint32_t want_h = 1080;

    get_wanted_mode_size(st, &want_w, &want_h);
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
    st->vblank_period_ns = compute_vblank_period_ns_internal(&s->mode);
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
