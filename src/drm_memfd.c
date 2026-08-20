/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "drm_memfd.h"
#include "drm_utils.h"
#include "memfd.h"
#include "stream.h"
#include "utils.h"

static gboolean
get_slot_base(StreamState                 *st,
              const MetaFuriosMemfdHeader *h,
              const uint8_t              **out_slot_base)
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
  if (off + (size_t)slot_bytes > st->memfd.map_len)
    return FALSE;

  *out_slot_base = (const uint8_t *)st->memfd.map_base + off;
  return TRUE;
}

static gboolean
copy_damage_direct_bxgx_to_back(StreamState                 *st,
                                DrmBuffer                   *front,
                                DrmBuffer                   *back,
                                const MetaFuriosMemfdHeader *h,
                                GArray                      *damage_rects)
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
    uint32_t seq_a = __atomic_load_n(&st->memfd.hdr->seq, __ATOMIC_ACQUIRE);

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

    uint32_t seq_b = __atomic_load_n(&st->memfd.hdr->seq, __ATOMIC_ACQUIRE);
    if (seq_a == seq_b)
      return TRUE;
  }

  return FALSE;
}

static void
ensure_cpu_buf(StreamState *st,
               size_t       need)
{
  if (!st)
    return;
  if (need == 0)
    return;
  if (st->memfd.cpu_buf && st->memfd.cpu_buf_len == need)
    return;

  g_free(st->memfd.cpu_buf);
  st->memfd.cpu_buf = g_malloc0(need);
  st->memfd.cpu_buf_len = need;
}

static gboolean
snapshot_slot_to_cpu(StreamState *st)
{
  if (!st || !st->memfd.hdr || !st->memfd.map_base)
    return FALSE;

  if (!memfd_header_sane(st->memfd.hdr))
    return FALSE;

  const uint32_t n_slots = st->memfd.hdr->n_slots ? st->memfd.hdr->n_slots : 1;
  const uint32_t slot_bytes = st->memfd.hdr->slot_bytes;
  const uint32_t header_bytes = st->memfd.hdr->header_bytes;

  if (slot_bytes == 0 || header_bytes < sizeof(MetaFuriosMemfdHeader))
    return FALSE;

  ensure_cpu_buf(st, (size_t)slot_bytes);
  if (!st->memfd.cpu_buf)
    return FALSE;

  for (int tries = 0; tries < 6; tries++) {
    uint32_t seq_a = __atomic_load_n(&st->memfd.hdr->seq, __ATOMIC_ACQUIRE);
    uint32_t slot = st->pending_slot % n_slots;

    size_t off = (size_t)header_bytes + (size_t)slot * (size_t)slot_bytes;
    if (off + (size_t)slot_bytes > st->memfd.map_len)
      return FALSE;

    const uint8_t *slot_base = (const uint8_t *)st->memfd.map_base + off;

    memcpy(st->memfd.cpu_buf, slot_base, (size_t)slot_bytes);

    uint32_t seq_b = __atomic_load_n(&st->memfd.hdr->seq, __ATOMIC_ACQUIRE);
    if (seq_a == seq_b)
      return TRUE;
  }

  return FALSE;
}

static void
blit_rect_rgba_to_xrgb8888(DrmBuffer                   *dst,
                           const MetaFuriosMemfdHeader *h,
                           const uint8_t               *src_full,
                           DamageRect                   r)
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
blit_damage_rgba_to_xrgb8888(DrmBuffer                   *dst,
                             const MetaFuriosMemfdHeader *h,
                             const uint8_t               *src_full,
                             GArray                      *damage_rects)
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

void
render_frame_drm_memfd(StreamState *st)
{
  if (!st || !st->memfd.hdr)
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
    r.w = (int32_t)st->memfd.hdr->width;
    r.h = (int32_t)st->memfd.hdr->height;
    g_array_append_val(st->pending_damage, r);
  }

  MetaFuriosMemfdHeader tmp = *st->memfd.hdr;

  if (tmp.width > s->fb_w)
    tmp.width = s->fb_w;
  if (tmp.height > s->fb_h)
    tmp.height = s->fb_h;

  int back_idx = 1 - s->front_idx;

  DrmBuffer *front = &s->bufs[s->front_idx];
  DrmBuffer *back = &s->bufs[back_idx];

  sanitize_damage_rects(st->pending_damage, tmp.width, tmp.height);

  if (tmp.format == META_FURIOS_MEMFD_FORMAT_BGRX8888 ||
      tmp.format == META_FURIOS_MEMFD_FORMAT_BGRA8888) {
    if (!copy_damage_direct_bxgx_to_back(st, front, back, &tmp, st->pending_damage))
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

    blit_damage_rgba_to_xrgb8888(back,
                                 &tmp,
                                 st->memfd.cpu_buf,
                                 st->pending_damage);
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
